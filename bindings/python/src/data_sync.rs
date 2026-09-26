//! Sync task admission and submission of the L3 data service, on the caller's
//! open transaction. Tasks and identities cross as JSON text; refusals raise
//! `ValueError`, concurrent or reused commands `Conflict`.
use crate::data_sources::DataCredentials;
use crate::database::{NativeConnection, NativeDatabaseError};
use crate::task_repository::TaskConflict;
use asterion_data::configuration::SourceError;
use asterion_data::sync::{self, Admission, DailyBatch, Submission};
use asterion_data_store::provider::SyncRequest;
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::database::Connection;
use asterion_kernel::tasks::repository::Repository;
use asterion_store::Transaction;
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};
use std::path::{Path, PathBuf};

fn error(value: SourceError) -> PyErr {
    match value {
        SourceError::Refused(message) => PyValueError::new_err(message),
        SourceError::Conflict(message) => TaskConflict::new_err(message),
        SourceError::Store(error) => NativeDatabaseError::new_err((
            if error.integrity {
                "integrity"
            } else {
                "operational"
            },
            error.message,
        )),
    }
}

fn invalid(error: impl ToString) -> PyErr {
    PyValueError::new_err(error.to_string())
}

fn parsed<T: DeserializeOwned>(text: &str) -> PyResult<T> {
    serde_json::from_str(text).map_err(invalid)
}

fn reader(root: &Path) -> PyResult<ArtifactStore> {
    ArtifactStore::reader(root, 1 << 30).map_err(invalid)
}

fn tasks(connection: &Connection) -> PyResult<Repository> {
    Repository::new(
        connection.database_id(),
        asterion_kernel::events::Topic {
            id: "runtime.task.changed".into(),
            owner: "asterion.runtime".into(),
            payload: "task".into(),
            read_path: "/jobs".into(),
        },
    )
    .map_err(invalid)
}

fn trace() -> PyResult<Value> {
    Ok(
        serde_json::to_value(
            asterion_kernel::communication::context(None, 600.0).map_err(invalid)?,
        )
        .expect("serializable context"),
    )
}

fn run<T: Serialize + Send>(
    py: Python<'_>,
    connection: &NativeConnection,
    work: impl FnOnce(&Connection) -> PyResult<T> + Send,
) -> PyResult<String> {
    py.detach(|| connection.with_connection(work))
        .map(|value| serde_json::to_string(&value).expect("serializable value"))
}

/// Check a task payload's request, type and fixed identity; the identity of
/// contract-bound tasks, otherwise null.
#[pyfunction]
fn data_sync_task_identity(payload: &str) -> PyResult<String> {
    let identity = sync::task_identity(&parsed(payload)?).map_err(error)?;
    Ok(identity
        .map_or(Value::Null, |resolver| json!(resolver.model()))
        .to_string())
}

#[pyfunction]
fn data_sync_validate_identity(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    payload: &str,
) -> PyResult<String> {
    let payload: Value = parsed(payload)?;
    let store = reader(&root)?;
    run(py, connection, |connection| {
        sync::validate_identity(&Transaction::over(connection), &store, &payload)
            .map(|resolver| json!(resolver.model()))
            .map_err(error)
    })
}

#[pyfunction]
fn data_sync_prepare(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    submission: &str,
) -> PyResult<String> {
    let submission: Submission = parsed(submission)?;
    let store = reader(&root)?;
    run(py, connection, |connection| {
        sync::prepare(&Transaction::over(connection), &store, &submission)
            .map(|(request, identity)| json!({"request": request, "identity": identity}))
            .map_err(error)
    })
}

#[pyfunction]
fn data_sync_payload(
    py: Python<'_>,
    connection: &NativeConnection,
    credentials: &DataCredentials,
    request: &str,
) -> PyResult<String> {
    let request: SyncRequest = parsed(request)?;
    run(py, connection, |connection| {
        sync::payload(&Transaction::over(connection), &credentials.inner, &request).map_err(error)
    })
}

#[pyfunction]
#[allow(clippy::too_many_arguments)]
fn data_sync_submit(
    py: Python<'_>,
    connection: &NativeConnection,
    credentials: &DataCredentials,
    root: PathBuf,
    request: &str,
    admission: &str,
    now: f64,
) -> PyResult<String> {
    let request: SyncRequest = parsed(request)?;
    let admission: Admission = parsed(admission)?;
    let store = reader(&root)?;
    run(py, connection, |connection| {
        let tasks = tasks(connection)?;
        sync::submit(
            &Transaction::over(connection),
            &store,
            &tasks,
            &credentials.inner,
            &request,
            &admission,
            now,
            &trace()?,
        )
        .map_err(error)
    })
}

#[pyfunction]
fn data_sync_admit(
    py: Python<'_>,
    connection: &NativeConnection,
    credentials: &DataCredentials,
    root: PathBuf,
    submission: &str,
    now: f64,
) -> PyResult<String> {
    let submission: Submission = parsed(submission)?;
    let store = reader(&root)?;
    run(py, connection, |connection| {
        let tasks = tasks(connection)?;
        sync::admit(
            &Transaction::over(connection),
            &store,
            &tasks,
            &credentials.inner,
            &submission,
            now,
            &trace()?,
        )
        .map_err(error)
    })
}

#[pyfunction]
fn data_sync_submit_batch(
    py: Python<'_>,
    connection: &NativeConnection,
    credentials: &DataCredentials,
    root: PathBuf,
    batch: &str,
    now: f64,
) -> PyResult<String> {
    let batch: DailyBatch = parsed(batch)?;
    let store = reader(&root)?;
    run(py, connection, |connection| {
        let tasks = tasks(connection)?;
        sync::submit_batch(
            &Transaction::over(connection),
            &store,
            &tasks,
            &credentials.inner,
            &batch,
            now,
            &trace()?,
        )
        .map_err(error)
    })
}

/// Collect a claimed sync task from its built-in source, as the entry's
/// collector does, against the database at `database_url`: the evidence it
/// publishes. Standalone acceptance tools run collection this way.
#[pyfunction]
fn data_sync_collect(
    py: Python<'_>,
    database_url: &str,
    credentials: &DataCredentials,
    root: PathBuf,
    job: &str,
    lease_seconds: f64,
) -> PyResult<Vec<u8>> {
    use asterion_data::collect::{CollectError, Host, collect};
    let job: Value = parsed(job)?;
    let token = job["token"].as_str().unwrap_or_default().to_string();
    py.detach(|| {
        let store = asterion_store::Store::open(database_url).map_err(invalid)?;
        let tasks = Repository::new(
            store.database_id(),
            asterion_kernel::events::Topic {
                id: "runtime.task.changed".into(),
                owner: "asterion.runtime".into(),
                payload: "task".into(),
                read_path: "/jobs".into(),
            },
        )
        .map_err(invalid)?;
        let artifacts = ArtifactStore::new(&root, 1 << 30).map_err(invalid)?;
        let trace = trace()?;
        let host = Host {
            store: &store,
            artifacts: &artifacts,
            tasks: &tasks,
            credentials: &credentials.inner,
            lease_seconds,
            trace: &trace,
        };
        let provider = job["payload"]["request"]["provider"]
            .as_str()
            .ok_or_else(|| PyValueError::new_err("同步请求格式不受支持"))
            .and_then(|id| asterion_data::providers::get(id).map_err(PyValueError::new_err))?;
        collect(&host, provider.as_ref(), &job, &token).map_err(|error| match error {
            CollectError::Failed(message) => PyValueError::new_err(message),
            CollectError::Lease(message) => TaskConflict::new_err(message),
        })
    })
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(data_sync_collect, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_task_identity, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_validate_identity, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_prepare, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_payload, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_submit, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_admit, m)?)?;
    m.add_function(wrap_pyfunction!(data_sync_submit_batch, m)?)?;
    Ok(())
}
