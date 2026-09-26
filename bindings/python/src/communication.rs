//! Strict Python value conversion; shared schema decisions stay in foundation.
use asterion_foundation::communication;
use asterion_kernel::communication as runtime;
use pyo3::{
    exceptions::{PyTimeoutError, PyValueError},
    prelude::*,
    types::{PyBool, PyDict, PyFloat, PyInt, PyList, PyString},
};
use serde_json::{Map, Number, Value};

pub(crate) fn value_from_python(
    value: &Bound<'_, PyAny>,
    ancestors: &mut Vec<usize>,
) -> PyResult<Value> {
    if ancestors.len() >= 128 {
        return Err(PyValueError::new_err(
            "Communication nesting exceeds budget",
        ));
    }
    if value.is_none() {
        return Ok(Value::Null);
    }
    if value.is_instance_of::<PyBool>() {
        return Ok(Value::Bool(value.extract()?));
    }
    if value.is_exact_instance_of::<PyInt>() {
        if let Ok(number) = value.extract::<i64>() {
            return Ok(Value::Number(number.into()));
        }
        return value
            .extract::<u64>()
            .map(|number| Value::Number(number.into()))
            .map_err(|_| PyValueError::new_err("Unsafe communication integer"));
    }
    if value.is_exact_instance_of::<PyFloat>() {
        return Number::from_f64(value.extract()?)
            .map(Value::Number)
            .ok_or_else(|| PyValueError::new_err("Non-finite communication number"));
    }
    if let Ok(text) = value.cast::<PyString>() {
        return Ok(Value::String(text.to_str()?.to_owned()));
    }
    let identity = value.as_ptr() as usize;
    if ancestors.contains(&identity) {
        return Err(PyValueError::new_err("Cyclic communication value"));
    }
    ancestors.push(identity);
    let result = if let Ok(map) = value.cast::<PyDict>() {
        let mut values = Map::new();
        for (key, child) in map.iter() {
            let key = key
                .cast::<PyString>()
                .map_err(|_| PyValueError::new_err("Invalid communication key"))?;
            values.insert(
                key.to_str()?.to_owned(),
                value_from_python(&child, ancestors)?,
            );
        }
        Ok(Value::Object(values))
    } else if let Ok(items) = value.cast::<PyList>() {
        let mut values = Vec::with_capacity(items.len());
        for child in items.iter() {
            values.push(value_from_python(&child, ancestors)?);
        }
        Ok(Value::Array(values))
    } else {
        Err(PyValueError::new_err("Non-JSON communication value"))
    };
    ancestors.pop();
    result
}

#[pyfunction]
fn communication_validate(name: &str, value: &Bound<'_, PyAny>) -> PyResult<()> {
    let value = value_from_python(value, &mut Vec::new())?;
    communication::validate(name, &value).map_err(PyValueError::new_err)
}

#[pyfunction]
fn communication_loads(raw: &str) -> PyResult<String> {
    let value = communication::parse_json(raw.as_bytes()).map_err(PyValueError::new_err)?;
    serde_json::to_string(&value).map_err(|error| PyValueError::new_err(error.to_string()))
}

fn failure(error: runtime::Error) -> PyErr {
    match error {
        runtime::Error::Deadline => PyTimeoutError::new_err(error.to_string()),
        _ => PyValueError::new_err(error.to_string()),
    }
}

fn encoded(value: &impl serde::Serialize) -> PyResult<String> {
    serde_json::to_string(value).map_err(|_| PyValueError::new_err("Invalid communication value"))
}

fn parent(raw: Option<&str>) -> PyResult<Option<asterion_foundation::wire_generated::Context>> {
    raw.map(|raw| {
        runtime::parse_context(
            communication::parse_json(raw.as_bytes()).map_err(PyValueError::new_err)?,
        )
        .map_err(failure)
    })
    .transpose()
}

#[pyfunction]
#[pyo3(signature = (timeout, raw_parent))]
fn communication_context(timeout: f64, raw_parent: Option<&str>) -> PyResult<String> {
    encoded(&runtime::context(parent(raw_parent)?.as_ref(), timeout).map_err(failure)?)
}

#[pyfunction]
fn communication_continue(
    correlation_id: &str,
    causation_id: &str,
    timeout: f64,
) -> PyResult<String> {
    encoded(&runtime::continuation(correlation_id, causation_id, timeout).map_err(failure)?)
}

#[pyfunction]
fn communication_active(value: &Bound<'_, PyAny>) -> PyResult<String> {
    let trace =
        runtime::parse_context(value_from_python(value, &mut Vec::new())?).map_err(failure)?;
    runtime::ensure_active(&trace).map_err(failure)?;
    encoded(&trace)
}

#[pyfunction]
#[pyo3(signature = (raw, timeout))]
fn communication_ingress(raw: Option<&str>, timeout: f64) -> PyResult<String> {
    encoded(&runtime::ingress(raw.map(str::as_bytes), timeout).map_err(failure)?)
}

#[pyfunction]
#[pyo3(signature = (timeout, raw_parent, kind, contract, payload))]
fn communication_call(
    timeout: f64,
    raw_parent: Option<&str>,
    kind: &str,
    contract: &str,
    payload: &Bound<'_, PyAny>,
) -> PyResult<String> {
    let trace = runtime::context(parent(raw_parent)?.as_ref(), timeout).map_err(failure)?;
    encoded(
        &runtime::prepare_call(
            &trace,
            kind,
            contract,
            value_from_python(payload, &mut Vec::new())?,
        )
        .map_err(failure)?,
    )
}

#[pyfunction]
fn communication_reply(
    trace: &Bound<'_, PyAny>,
    result: &Bound<'_, PyAny>,
    error: &Bound<'_, PyAny>,
) -> PyResult<String> {
    let trace =
        runtime::parse_context(value_from_python(trace, &mut Vec::new())?).map_err(failure)?;
    encoded(
        &runtime::prepare_reply(
            &trace,
            value_from_python(result, &mut Vec::new())?,
            value_from_python(error, &mut Vec::new())?,
        )
        .map_err(failure)?,
    )
}

#[pyfunction]
fn communication_result(trace: &Bound<'_, PyAny>, response: &Bound<'_, PyAny>) -> PyResult<String> {
    let trace =
        runtime::parse_context(value_from_python(trace, &mut Vec::new())?).map_err(failure)?;
    encoded(
        &runtime::reply_result(&trace, value_from_python(response, &mut Vec::new())?)
            .map_err(failure)?,
    )
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_function(wrap_pyfunction!(communication_validate, module)?)?;
    module.add_function(wrap_pyfunction!(communication_loads, module)?)?;
    module.add_function(wrap_pyfunction!(communication_context, module)?)?;
    module.add_function(wrap_pyfunction!(communication_continue, module)?)?;
    module.add_function(wrap_pyfunction!(communication_active, module)?)?;
    module.add_function(wrap_pyfunction!(communication_ingress, module)?)?;
    module.add_function(wrap_pyfunction!(communication_call, module)?)?;
    module.add_function(wrap_pyfunction!(communication_reply, module)?)?;
    module.add_function(wrap_pyfunction!(communication_result, module)?)?;
    Ok(())
}
