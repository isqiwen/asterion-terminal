//! Minute bars bound to fixed trading-time evidence (L2 data store). Contexts,
//! rows and instants cross as JSON text; refusals raise `ValueError`.
use asterion_data_store::minute::Minute;
use asterion_data_store::table::Row;
use chrono::NaiveDate;
use pyo3::{exceptions::PyValueError, prelude::*};
use serde_json::Value;

fn minute(context: &str) -> PyResult<Minute> {
    let value: Value =
        serde_json::from_str(context).map_err(|error| PyValueError::new_err(error.to_string()))?;
    Minute::from_value(&value).map_err(PyValueError::new_err)
}

fn day(text: &str) -> PyResult<NaiveDate> {
    NaiveDate::parse_from_str(text, "%Y-%m-%d")
        .map_err(|_| PyValueError::new_err("交易日格式不正确"))
}

/// The validated context, with its source description trimmed.
#[pyfunction]
fn data_minute_context(context: &str) -> PyResult<String> {
    Ok(serde_json::to_string(&minute(context)?.context).expect("serializable context"))
}

#[pyfunction]
fn data_minute_labels(context: &str, contract: &str, trading_day: &str) -> PyResult<String> {
    let labels = minute(context)?
        .label_stamps(contract, day(trading_day)?)
        .map_err(PyValueError::new_err)?;
    let texts: Vec<String> = labels.iter().map(|label| label.to_rfc3339()).collect();
    Ok(serde_json::to_string(&texts).expect("serializable labels"))
}

#[pyfunction]
fn data_minute_window(context: &str, contract: &str, trading_day: &str) -> PyResult<String> {
    let window = minute(context)?
        .window(contract, day(trading_day)?)
        .map_err(PyValueError::new_err)?;
    Ok(serde_json::to_string(&window).expect("serializable window"))
}

#[pyfunction]
fn data_minute_bind(context: &str, rows: &str, observed: &str) -> PyResult<String> {
    let minute = minute(context)?;
    let mut rows: Vec<Row> =
        serde_json::from_str(rows).map_err(|error| PyValueError::new_err(error.to_string()))?;
    minute
        .bind_rows(&mut rows, observed)
        .map_err(PyValueError::new_err)?;
    Ok(serde_json::to_string(&rows).expect("serializable rows"))
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(data_minute_context, m)?)?;
    m.add_function(wrap_pyfunction!(data_minute_labels, m)?)?;
    m.add_function(wrap_pyfunction!(data_minute_window, m)?)?;
    m.add_function(wrap_pyfunction!(data_minute_bind, m)?)?;
    Ok(())
}
