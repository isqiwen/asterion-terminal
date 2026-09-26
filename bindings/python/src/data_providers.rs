//! The built-in data sources of the L3 data service. Values cross as JSON
//! text; refusals raise `ValueError` with the user-facing reason.
use asterion_data::providers;
use asterion_data_store::provider::{Partition, SyncRequest, checked_plan};
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::de::DeserializeOwned;
use serde_json::{Map, Value};

fn parsed<T: DeserializeOwned>(text: &str) -> PyResult<T> {
    serde_json::from_str(text).map_err(|error| PyValueError::new_err(error.to_string()))
}

fn json(value: &impl serde::Serialize) -> String {
    serde_json::to_string(value).expect("serializable provider value")
}

#[pyfunction]
fn data_provider_manifests() -> String {
    json(&providers::manifests())
}

/// The admitted partitions of a built-in provider.
#[pyfunction]
fn data_provider_plan(provider: &str, request: &str) -> PyResult<String> {
    let request: SyncRequest = parsed(request)?;
    let provider = providers::get(provider).map_err(PyValueError::new_err)?;
    providers::plan(provider.as_ref(), &request)
        .map(|parts| json(&parts))
        .map_err(PyValueError::new_err)
}

/// The same admission rules for a plan produced elsewhere.
#[pyfunction]
fn data_provider_check_plan(request: &str, parts: &str) -> PyResult<()> {
    let request: SyncRequest = parsed(request)?;
    let parts: Vec<Partition> = parsed(parts)?;
    checked_plan(&request, &parts).map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_provider_probe(py: Python<'_>, provider: &str, configuration: &str) -> PyResult<String> {
    let configuration: Map<String, Value> = parsed(configuration)?;
    let provider = providers::get(provider).map_err(PyValueError::new_err)?;
    py.detach(|| provider.probe(&configuration))
        .map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_provider_fetch(
    py: Python<'_>,
    provider: &str,
    partition: &str,
    configuration: &str,
) -> PyResult<String> {
    let partition: Partition = parsed(partition)?;
    let configuration: Map<String, Value> = parsed(configuration)?;
    let provider = providers::get(provider).map_err(PyValueError::new_err)?;
    py.detach(|| provider.fetch(&partition, &configuration))
        .map(|rows| json(&rows))
        .map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_provider_normalize(provider: &str, request: &str, rows: &str) -> PyResult<String> {
    let request: SyncRequest = parsed(request)?;
    let rows: Vec<Value> = parsed(rows)?;
    let provider = providers::get(provider).map_err(PyValueError::new_err)?;
    provider
        .normalize(&request, &rows)
        .map(|rows| json(&rows))
        .map_err(PyValueError::new_err)
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(data_provider_manifests, m)?)?;
    m.add_function(wrap_pyfunction!(data_provider_plan, m)?)?;
    m.add_function(wrap_pyfunction!(data_provider_check_plan, m)?)?;
    m.add_function(wrap_pyfunction!(data_provider_probe, m)?)?;
    m.add_function(wrap_pyfunction!(data_provider_fetch, m)?)?;
    m.add_function(wrap_pyfunction!(data_provider_normalize, m)?)?;
    Ok(())
}
