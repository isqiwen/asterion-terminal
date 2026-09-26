//! Source observations of sync tasks, from the L3 data service, on the
//! caller's open transaction. Refusals raise `ValueError`, lease and reuse
//! conflicts `Conflict`, unknown tasks or observations `KeyError`.
use crate::database::{NativeConnection, NativeDatabaseError};
use crate::task_repository::TaskConflict;
use asterion_data::observations::{self, EvidenceError, Observation};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::tasks::repository::Repository;
use asterion_store::Transaction;
use pyo3::{
    exceptions::{PyKeyError, PyValueError},
    prelude::*,
};
use serde::Serialize;
use serde_json::Value;
use std::path::PathBuf;

fn error(value: EvidenceError) -> PyErr {
    match value {
        EvidenceError::Refused(message) => PyValueError::new_err(message),
        EvidenceError::Conflict(message) => TaskConflict::new_err(message),
        EvidenceError::NotFound => PyKeyError::new_err("observation"),
        EvidenceError::Store(error) => NativeDatabaseError::new_err((
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

fn store(root: &std::path::Path, writable: bool) -> PyResult<ArtifactStore> {
    let open = if writable {
        ArtifactStore::new
    } else {
        ArtifactStore::reader
    };
    open(root, 1 << 30).map_err(invalid)
}

fn tasks(connection: &asterion_kernel::database::Connection) -> PyResult<Repository> {
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
    work: impl FnOnce(&asterion_kernel::database::Connection) -> PyResult<T> + Send,
) -> PyResult<String> {
    py.detach(|| connection.with_connection(work))
        .map(|value| serde_json::to_string(&value).expect("serializable value"))
}

fn observation(value: &str) -> PyResult<Observation> {
    let value: Value = serde_json::from_str(value).map_err(invalid)?;
    Observation::parse(&value).map_err(error)
}

#[pyfunction]
#[allow(clippy::too_many_arguments)]
fn data_observation_record(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    job_id: &str,
    token: &str,
    index: i64,
    value: &str,
    now: f64,
) -> PyResult<String> {
    let value = observation(value)?;
    let artifacts = store(&root, true)?;
    run(py, connection, |connection| {
        let tasks = tasks(connection)?;
        let tx = Transaction::over(connection);
        observations::record(
            &tx,
            &artifacts,
            &tasks,
            job_id,
            token,
            index,
            &value,
            now,
            &trace()?,
        )
        .map_err(error)
    })
}

#[pyfunction]
fn data_observation_resume(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    job_id: &str,
    token: &str,
    now: f64,
) -> PyResult<String> {
    let artifacts = store(&root, true)?;
    run(py, connection, |connection| {
        let tasks = tasks(connection)?;
        let tx = Transaction::over(connection);
        observations::resume(&tx, &artifacts, &tasks, job_id, token, now, &trace()?).map_err(error)
    })
}

#[pyfunction]
fn data_observation_verify(
    py: Python<'_>,
    connection: &NativeConnection,
    job_id: &str,
    attempt: i64,
    envelope: &str,
) -> PyResult<String> {
    let envelope: Vec<Value> = serde_json::from_str(envelope).map_err(invalid)?;
    run(py, connection, |connection| {
        observations::verify_publication(&Transaction::over(connection), job_id, attempt, &envelope)
            .map_err(error)
    })
}

#[pyfunction]
fn data_observation_list(
    py: Python<'_>,
    connection: &NativeConnection,
    job_id: &str,
    offset: i64,
    limit: i64,
) -> PyResult<String> {
    run(py, connection, |connection| {
        observations::list(&Transaction::over(connection), job_id, offset, limit).map_err(error)
    })
}

#[pyfunction]
#[allow(clippy::too_many_arguments)]
fn data_observation_preview(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    job_id: &str,
    attempt: i64,
    index: i64,
    offset: usize,
    limit: usize,
) -> PyResult<String> {
    let artifacts = store(&root, false)?;
    run(py, connection, |connection| {
        observations::preview(
            &Transaction::over(connection),
            &artifacts,
            job_id,
            attempt,
            index,
            offset,
            limit,
        )
        .map_err(error)
    })
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(data_observation_record, m)?)?;
    m.add_function(wrap_pyfunction!(data_observation_resume, m)?)?;
    m.add_function(wrap_pyfunction!(data_observation_verify, m)?)?;
    m.add_function(wrap_pyfunction!(data_observation_list, m)?)?;
    m.add_function(wrap_pyfunction!(data_observation_preview, m)?)?;
    Ok(())
}
