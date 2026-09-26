//! Python object handles for the fixed task execution resource scope.
use asterion_kernel::{
    plugins::Resource,
    tasks::{ExecutionHandle, ExecutionScope},
};
use pyo3::{exceptions::PyValueError, prelude::*};

#[pyclass(name = "ExecutionHandle", module = "asterion_bindings._native")]
pub(crate) struct PyHandle {
    pub(crate) handle: ExecutionHandle,
}
#[pymethods]
impl PyHandle {
    fn check(&self) -> PyResult<()> {
        self.handle.check().map_err(PyValueError::new_err)
    }
}

#[pyclass(name = "ExecutionScope", module = "asterion_bindings._native")]
struct PyScope {
    scope: ExecutionScope,
}
#[pymethods]
impl PyScope {
    #[new]
    fn new(declarations: &str, grants: &str) -> PyResult<Self> {
        let declarations: Vec<Resource> = serde_json::from_str(declarations)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        let grants: Vec<Resource> = serde_json::from_str(grants)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        Ok(Self {
            scope: ExecutionScope::new(declarations, grants).map_err(PyValueError::new_err)?,
        })
    }
    fn resource(&self, value: &str) -> PyResult<PyHandle> {
        let resource: Resource = serde_json::from_str(value)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        self.scope
            .resource(&resource)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn close(&mut self) {
        self.scope.close();
    }
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyScope>()?;
    module.add_class::<PyHandle>()?;
    Ok(())
}
