//! Value and ownership conversion only; mechanisms and domain rules live below.
use pyo3::exceptions::PyValueError;
use pyo3::prelude::*;
mod archives;
mod communication;
mod connections;
mod data_catalog;
mod data_identity;
mod data_minute;
mod data_observations;
mod data_providers;
mod data_sources;
mod data_store;
mod data_sync;
mod database;
mod diagnostics;
mod directories;
mod domain;
mod environments;
mod events;
mod execution;
mod file_archives;
mod files;
mod host;
mod lifetimes;
mod market_feed;
mod recovery;
mod secrets;
mod storage;
mod supervisor;
mod task_handlers;
mod task_process;
mod task_repository;
mod tasks;
mod transport;

#[pyfunction]
fn invoke(py: Python<'_>, module: &str, operation: &str, input: &str) -> PyResult<String> {
    py.detach(|| {
        let value = if module == "connections" {
            if input.len() > asterion_connections::MAX_STATE_BYTES {
                return Err("Connection contract exceeds byte budget".into());
            }
            asterion_foundation::communication::parse_json(input.as_bytes())?
        } else {
            serde_json::from_str(input).map_err(|e| e.to_string())?
        };
        let result = match module {
            "kernel" => asterion_kernel::invoke(operation, value),
            "catalog" => asterion_instrument_catalog::invoke(operation, value),
            "calendar" => asterion_trading_calendar::invoke(operation, value),
            "rules" => asterion_market_rules::invoke(operation, value),
            "roles" => asterion_role_registry::invoke(operation, value),
            "market_feed" => asterion_market_feed::invoke(operation, value),
            "connections" => asterion_connections::invoke(operation, value),
            "data_store" => asterion_data_store::invoke(operation, value),
            "execution" => asterion_execution::invoke(operation, value),
            _ => Err("unknown native module".into()),
        }?;
        serde_json::to_string(&result).map_err(|e| e.to_string())
    })
    .map_err(PyValueError::new_err)
}

#[pymodule]
fn _native(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(invoke, m)?)?;
    host::register(m)?;
    connections::register(m)?;
    data_store::register(m)?;
    data_catalog::register(m)?;
    data_providers::register(m)?;
    data_sources::register(m)?;
    data_observations::register(m)?;
    data_minute::register(m)?;
    data_identity::register(m)?;
    data_sync::register(m)?;
    execution::register(m)?;
    market_feed::register(m)?;
    archives::register(m)?;
    tasks::register(m)?;
    domain::register(m)?;
    environments::register(m)?;
    communication::register(m)?;
    database::register(m)?;
    diagnostics::register(m)?;
    directories::register(m)?;
    events::register(m)?;
    files::register(m)?;
    file_archives::register(m)?;
    recovery::register(m)?;
    secrets::register(m)?;
    storage::register(m)?;
    supervisor::register(m)?;
    task_handlers::register(m)?;
    task_process::register(m)?;
    task_repository::register(m)?;
    transport::register(m)?;
    Ok(())
}
