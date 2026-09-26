//! The binding owns Python callbacks, never environment transition decisions.
use asterion_kernel::environments::{Action, AdapterError, Error, Layout, Lease};
use pyo3::{
    exceptions::{PyException, PyValueError},
    prelude::*,
};
use serde_json::Value;
use std::{path::PathBuf, sync::Mutex};

#[pyclass(name = "EnvironmentLease", module = "asterion_bindings._native")]
struct PyLease {
    lease: Mutex<Option<Lease>>,
}

fn error(value: Error<PyErr>) -> PyErr {
    match value {
        Error::Mechanism(message) => PyValueError::new_err(message),
        Error::Adapter(error) => error.error,
    }
}
fn invoke(callback: &Py<PyAny>, action: Action<'_>) -> Result<Value, AdapterError<PyErr>> {
    Python::attach(|py| {
        let result = (|| {
            let raw = serde_json::to_string(&action)
                .map_err(|_| PyValueError::new_err("Invalid environment operation"))?;
            let value = callback.call1(py, (raw,))?.extract::<String>(py)?;
            serde_json::from_str(&value)
                .map_err(|_| PyValueError::new_err("Invalid environment adapter result"))
        })();
        result.map_err(|error: PyErr| {
            let interrupted = !error.is_instance_of::<PyException>(py);
            AdapterError { error, interrupted }
        })
    })
}
impl PyLease {
    fn with_lease<T: Send>(
        &self,
        py: Python<'_>,
        operation: impl FnOnce(&Lease) -> PyResult<T> + Send,
    ) -> PyResult<T> {
        py.detach(|| {
            let lease = self
                .lease
                .try_lock()
                .map_err(|_| PyValueError::new_err("Environment operation is already running"))?;
            operation(
                lease
                    .as_ref()
                    .ok_or_else(|| PyValueError::new_err("Environment lease is closed"))?,
            )
        })
    }
}
#[pymethods]
impl PyLease {
    #[new]
    fn new(
        py: Python<'_>,
        host: PathBuf,
        backup_directory: PathBuf,
        layout: &str,
        wait: bool,
    ) -> PyResult<Self> {
        let layout: Layout = serde_json::from_str(layout)
            .map_err(|_| PyValueError::new_err("Invalid environment layout"))?;
        let lease = py
            .detach(|| Lease::acquire(&host, &backup_directory, layout, wait))
            .map_err(PyValueError::new_err)?;
        Ok(Self {
            lease: Mutex::new(Some(lease)),
        })
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .try_lock()
                .map_err(|_| PyValueError::new_err("Environment operation is already running"))?
                .take();
            Ok(())
        })
    }
    fn active(&self, py: Python<'_>) -> PyResult<PathBuf> {
        self.with_lease(py, |lease| lease.active().map_err(PyValueError::new_err))
    }
    fn status(&self, py: Python<'_>) -> PyResult<String> {
        self.with_lease(py, |lease| {
            let value = lease.read().map_err(PyValueError::new_err)?;
            serde_json::to_string(&value)
                .map_err(|_| PyValueError::new_err("Invalid environment record"))
        })
    }
    fn recover(&self, py: Python<'_>, callback: Py<PyAny>) -> PyResult<()> {
        self.with_lease(py, |lease| {
            lease
                .recover(&mut |action| invoke(&callback, action))
                .map_err(error)
        })
    }
    fn switch(
        &self,
        py: Python<'_>,
        target: Option<PathBuf>,
        callback: Py<PyAny>,
    ) -> PyResult<String> {
        self.with_lease(py, |lease| {
            let result = lease
                .switch(target.as_deref(), &mut |action| invoke(&callback, action))
                .map_err(error)?;
            serde_json::to_string(&result)
                .map_err(|_| PyValueError::new_err("Invalid environment result"))
        })
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyLease>()
}
