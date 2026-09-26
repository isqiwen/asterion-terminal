//! Extract fixed native ownership handles, never arbitrary Python guard callbacks.
use asterion_kernel::lifetime::Lifetime;
use pyo3::{exceptions::PyValueError, prelude::*};

pub(crate) fn one(value: &Bound<'_, PyAny>) -> PyResult<Lifetime> {
    if let Ok(value) = value.extract::<PyRef<'_, crate::host::PyHandle>>() {
        Ok(value.handle.clone().into())
    } else if let Ok(value) = value.extract::<PyRef<'_, crate::tasks::PyHandle>>() {
        Ok(value.handle.clone().into())
    } else {
        Err(PyValueError::new_err("Expected a native resource lifetime"))
    }
}

pub(crate) fn parse(py: Python<'_>, values: Vec<Py<PyAny>>) -> PyResult<Vec<Lifetime>> {
    if values.len() > 128 {
        return Err(PyValueError::new_err(
            "Resource lifetime nesting exceeds its budget",
        ));
    }
    let scopes = values
        .into_iter()
        .map(|value| one(value.bind(py)))
        .collect::<PyResult<Vec<_>>>()?;
    for scope in &scopes {
        scope.check().map_err(PyValueError::new_err)?;
    }
    Ok(scopes)
}
