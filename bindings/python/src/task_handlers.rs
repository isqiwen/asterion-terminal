//! Python callback declarations backed by the fixed Rust task registry.
use asterion_kernel::tasks::handlers::{Declaration, Registry, RequestScope};
use pyo3::{exceptions::PyValueError, prelude::*};

#[pyclass(
    name = "TaskHandlerDeclaration",
    module = "asterion_bindings._native",
    frozen
)]
struct PyDeclaration {
    declaration: Declaration,
}

#[pymethods]
impl PyDeclaration {
    #[new]
    fn new(kind: String, publish_suffix: String, requests: &str) -> PyResult<Self> {
        let requests = serde_json::from_str(requests)
            .map_err(|error| PyValueError::new_err(error.to_string()))?;
        Ok(Self {
            declaration: Declaration::new(kind, publish_suffix, requests)
                .map_err(PyValueError::new_err)?,
        })
    }

    fn requests(&self) -> PyRequestScope {
        PyRequestScope {
            scope: self.declaration.requests(),
        }
    }
}

#[pyclass(
    name = "TaskRequestScope",
    module = "asterion_bindings._native",
    frozen
)]
struct PyRequestScope {
    scope: RequestScope,
}

#[pymethods]
impl PyRequestScope {
    fn authorize(&self, suffix: &str) -> PyResult<()> {
        self.scope.authorize(suffix).map_err(PyValueError::new_err)
    }

    fn close(&self) {
        self.scope.close();
    }
}

#[pyclass(name = "TaskHandlerRegistry", module = "asterion_bindings._native")]
#[derive(Default)]
struct PyRegistry {
    registry: Registry,
}

#[pymethods]
impl PyRegistry {
    #[new]
    fn new() -> Self {
        Self::default()
    }

    fn register(&mut self, declaration: &PyDeclaration) -> PyResult<usize> {
        self.registry
            .register(declaration.declaration.clone())
            .map_err(PyValueError::new_err)
    }

    fn get(&self, kind: &str) -> PyResult<usize> {
        self.registry.get(kind).map_err(PyValueError::new_err)
    }

    fn worker_grants(&self) -> PyResult<String> {
        serde_json::to_string(&self.registry.worker_grants())
            .map_err(|error| PyValueError::new_err(error.to_string()))
    }
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyDeclaration>()?;
    module.add_class::<PyRegistry>()?;
    module.add_class::<PyRequestScope>()?;
    Ok(())
}
