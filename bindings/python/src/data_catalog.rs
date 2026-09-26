//! The data catalogue queries of the L3 data service, run on the caller's open
//! transaction. Values cross as JSON text.
use crate::database::{NativeConnection, NativeDatabaseError};
use asterion_data::catalog::{self, CatalogError, CatalogQuery};
use asterion_data::preview::{PreviewError, preview};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_store::{StoreError, Transaction};
use pyo3::{
    exceptions::{PyKeyError, PyValueError},
    prelude::*,
};
use std::path::PathBuf;

fn error(value: CatalogError) -> PyErr {
    match value {
        CatalogError::Invalid(message) => PyValueError::new_err(message),
        CatalogError::Store(error) => stored(error),
    }
}

fn stored(error: StoreError) -> PyErr {
    NativeDatabaseError::new_err((
        if error.integrity {
            "integrity"
        } else {
            "operational"
        },
        error.message,
    ))
}

/// Rows of one published version; unknown versions raise `KeyError`.
#[pyfunction]
fn data_version_preview(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    version_id: &str,
    offset: usize,
    limit: usize,
) -> PyResult<String> {
    py.detach(|| {
        let store = ArtifactStore::reader(&root, 1 << 30)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        connection.with_connection(|connection| {
            preview(
                &Transaction::over(connection),
                &store,
                version_id,
                offset,
                limit,
            )
            .map_err(|error| match error {
                PreviewError::NotFound => PyKeyError::new_err(version_id.to_string()),
                PreviewError::Refused(message) => PyValueError::new_err(message),
                PreviewError::Store(error) => stored(error),
            })
        })
    })
    .map(|value| serde_json::to_string(&value).expect("serializable preview"))
}

fn run(
    py: Python<'_>,
    connection: &NativeConnection,
    work: impl FnOnce(&Transaction) -> Result<serde_json::Value, CatalogError> + Send,
) -> PyResult<String> {
    py.detach(|| {
        connection.with_connection(|connection| work(&Transaction::over(connection)).map_err(error))
    })
    .map(|value| value.to_string())
}

#[pyfunction]
fn data_catalog_list(
    py: Python<'_>,
    connection: &NativeConnection,
    query: &str,
) -> PyResult<String> {
    let query: CatalogQuery = serde_json::from_str(query)
        .map_err(|_| PyValueError::new_err("Invalid catalogue query"))?;
    run(py, connection, |tx| catalog::list(tx, &query))
}

#[pyfunction]
fn data_catalog_history(
    py: Python<'_>,
    connection: &NativeConnection,
    dataset_id: &str,
    offset: i64,
    limit: i64,
) -> PyResult<String> {
    run(py, connection, |tx| {
        catalog::history(tx, dataset_id, offset, limit)
    })
}

#[pyfunction]
fn data_catalog_hierarchy(
    py: Python<'_>,
    connection: &NativeConnection,
    include_archived: bool,
) -> PyResult<String> {
    run(py, connection, |tx| {
        Ok(serde_json::to_value(catalog::hierarchy(tx, include_archived)?).expect("serializable"))
    })
}

/// Execute a claimed import task on the caller's write transaction, as the
/// entry's executor does; lease loss raises KeyError, refusals ValueError.
#[pyfunction]
fn data_import_execute(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    job_id: &str,
    token: &str,
    now: f64,
) -> PyResult<String> {
    use asterion_data::imports::{ExecuteError, execute};
    py.detach(|| {
        let store = ArtifactStore::new(&root, 1 << 30)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        connection.with_connection(|connection| {
            let tasks = asterion_kernel::tasks::repository::Repository::new(
                connection.database_id(),
                asterion_kernel::events::Topic {
                    id: "runtime.task.changed".into(),
                    owner: "asterion.runtime".into(),
                    payload: "task".into(),
                    read_path: "/jobs".into(),
                },
            )
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
            let trace = serde_json::to_value(
                asterion_kernel::communication::context(None, 600.0)
                    .map_err(|error| PyValueError::new_err(error.to_string()))?,
            )
            .expect("serializable context");
            execute(
                &Transaction::over(connection),
                &store,
                &tasks,
                job_id,
                token,
                trace,
                now,
            )
            .map_err(|error| match error {
                ExecuteError::Lease(message) => PyKeyError::new_err(message),
                ExecuteError::Refused(message) => PyValueError::new_err(message),
            })
        })
    })
    .map(|value| value.to_string())
}

/// Chart bars of a published snapshot; unknown snapshots raise KeyError.
#[pyfunction]
fn data_snapshot_bars(
    py: Python<'_>,
    connection: &NativeConnection,
    root: PathBuf,
    snapshot_id: &str,
    limit: usize,
) -> PyResult<String> {
    use asterion_data::snapshots::{SnapshotError, bars};
    py.detach(|| {
        let store = ArtifactStore::reader(&root, 1 << 30)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        connection.with_connection(|connection| {
            bars(&Transaction::over(connection), &store, snapshot_id, limit).map_err(|error| {
                match error {
                    SnapshotError::NotFound => PyKeyError::new_err(snapshot_id.to_string()),
                    SnapshotError::Refused(message) => PyValueError::new_err(message),
                    SnapshotError::Store(error) => stored(error),
                }
            })
        })
    })
    .map(|value| serde_json::Value::Array(value).to_string())
}

/// The daily chart table of daily rows (JSON) and its manifest (JSON).
#[pyfunction]
fn data_daily_chart<'py>(
    py: Python<'py>,
    rows: &str,
    available_at: &str,
) -> PyResult<(Bound<'py, pyo3::types::PyBytes>, String)> {
    let rows: Vec<serde_json::Value> =
        serde_json::from_str(rows).map_err(|error| PyValueError::new_err(error.to_string()))?;
    let (content, manifest) = py
        .detach(|| asterion_data_store::bars::daily_chart(&rows, available_at))
        .map_err(PyValueError::new_err)?;
    Ok((
        pyo3::types::PyBytes::new(py, &content),
        manifest.to_string(),
    ))
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_function(wrap_pyfunction!(data_import_execute, module)?)?;
    module.add_function(wrap_pyfunction!(data_snapshot_bars, module)?)?;
    module.add_function(wrap_pyfunction!(data_daily_chart, module)?)?;
    module.add_function(wrap_pyfunction!(data_catalog_list, module)?)?;
    module.add_function(wrap_pyfunction!(data_catalog_history, module)?)?;
    module.add_function(wrap_pyfunction!(data_catalog_hierarchy, module)?)?;
    module.add_function(wrap_pyfunction!(data_version_preview, module)?)?;
    Ok(())
}
