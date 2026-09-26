//! Python callback/type adaptation for fixed recovery state owned by L1.
use asterion_kernel::recovery::{self, InvokeError, Metrics, Operation, Scope};
use pyo3::{
    exceptions::{PyTypeError, PyValueError},
    prelude::*,
};
use std::collections::BTreeMap;

#[pyclass(name = "RestoreScope", frozen, module = "asterion_bindings._native")]
struct PyScope {
    inner: Scope,
}
#[pymethods]
impl PyScope {
    #[new]
    fn new() -> Self {
        Self {
            inner: Scope::new(),
        }
    }
    fn operation(&self, action: Py<PyAny>, py: Python<'_>) -> PyResult<PyOperation> {
        if !action.bind(py).is_callable() {
            return Err(PyTypeError::new_err("Restore operation must be callable"));
        }
        Ok(PyOperation {
            inner: self.inner.operation().map_err(PyValueError::new_err)?,
            action,
        })
    }
    fn verify(&self) -> PyResult<()> {
        self.inner.verify().map_err(PyValueError::new_err)
    }
    fn close(&self) {
        self.inner.close();
    }
}

#[pyclass(
    name = "RestoreOperation",
    frozen,
    module = "asterion_bindings._native"
)]
struct PyOperation {
    inner: Operation,
    action: Py<PyAny>,
}
#[pymethods]
impl PyOperation {
    fn __call__(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.inner.invoke(|| {
                Python::attach(|py| {
                    let result = self.action.call0(py)?;
                    let inspect = py.import("inspect")?;
                    if inspect
                        .call_method1("isawaitable", (&result,))?
                        .is_truthy()?
                    {
                        // An unstarted owned coroutine can be closed without running
                        // it. Other awaitables are rejected without changing them.
                        if inspect
                            .call_method1("iscoroutine", (&result,))?
                            .is_truthy()?
                        {
                            result.call_method0(py, "close")?;
                        }
                        return Err(PyValueError::new_err(
                            "Restore operations must complete synchronously",
                        ));
                    }
                    Ok(())
                })
            })
        })
        .map_err(|error| match error {
            InvokeError::Mechanism(message) => PyValueError::new_err(message),
            InvokeError::Callback(error) => error,
        })
    }
}

#[pyclass(name = "BackupMetrics", module = "asterion_bindings._native")]
struct PyMetrics {
    inner: Metrics,
}
#[pymethods]
impl PyMetrics {
    #[new]
    fn new() -> Self {
        Self {
            inner: Metrics::default(),
        }
    }
    fn add(&mut self, value: &Bound<'_, PyAny>) -> PyResult<()> {
        let value = crate::communication::value_from_python(value, &mut Vec::new())
            .map_err(|_| PyValueError::new_err("Invalid backup validation metrics"))?;
        self.inner.add(value).map_err(PyValueError::new_err)
    }
    fn counts(&self) -> BTreeMap<String, u64> {
        self.inner.counts().clone()
    }
}

#[pyfunction]
fn recovery_inputs(expected: Vec<String>, provided: Vec<String>, restore: bool) -> PyResult<()> {
    recovery::inputs(&expected, &provided, restore).map_err(PyValueError::new_err)
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyScope>()?;
    module.add_class::<PyOperation>()?;
    module.add_class::<PyMetrics>()?;
    module.add_function(wrap_pyfunction!(recovery_inputs, module)?)?;
    Ok(())
}
