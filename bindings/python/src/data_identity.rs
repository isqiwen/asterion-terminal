//! Identity catalogs from fixed standard contracts versions (L2 data store).
//! Version previews, catalogs and contracts cross as JSON text; refusals raise
//! `ValueError` with the user-facing reason.
use asterion_data_store::identity;
use asterion_instrument_catalog::CatalogInput;
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::de::DeserializeOwned;
use serde_json::{Value, json};

fn parsed<T: DeserializeOwned>(text: &str) -> PyResult<T> {
    serde_json::from_str(text).map_err(|error| PyValueError::new_err(error.to_string()))
}

#[pyfunction]
fn data_catalog_input_check(item: &str, manifest: &str) -> PyResult<()> {
    let item: CatalogInput = parsed(item)?;
    identity::validate_catalog_input(&item, &parsed(manifest)?).map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_source_catalog(preview: &str, version_id: &str, symbols: &str) -> PyResult<String> {
    let symbols: Vec<String> = parsed(symbols)?;
    identity::source_catalog(&parsed::<Value>(preview)?, version_id, &symbols)
        .map(|catalog| serde_json::to_string(&catalog).expect("serializable catalog"))
        .map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_product_catalog(preview: &str, version_id: &str, product_id: &str) -> PyResult<String> {
    identity::product_catalog(&parsed::<Value>(preview)?, version_id, product_id)
        .map(|catalog| serde_json::to_string(&catalog).expect("serializable catalog"))
        .map_err(PyValueError::new_err)
}

#[pyfunction]
fn data_source_contract(preview: &str, contract_id: &str) -> PyResult<String> {
    identity::source_contract(&parsed::<Value>(preview)?, contract_id)
        .map(|(contract, row)| json!({"contract": contract, "row": row}).to_string())
        .map_err(PyValueError::new_err)
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(data_catalog_input_check, m)?)?;
    m.add_function(wrap_pyfunction!(data_source_catalog, m)?)?;
    m.add_function(wrap_pyfunction!(data_product_catalog, m)?)?;
    m.add_function(wrap_pyfunction!(data_source_contract, m)?)?;
    Ok(())
}
