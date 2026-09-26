//! SQLAlchemy object identity and statement-description adapters for native grants.
use asterion_kernel::storage::{self, Scope, Transaction};
use pyo3::{exceptions::PyValueError, prelude::*};
use std::sync::Arc;

fn value<T: serde::de::DeserializeOwned>(raw: &str) -> PyResult<T> {
    serde_json::from_str(raw).map_err(|_| PyValueError::new_err("Invalid storage descriptor"))
}
#[pyclass(name = "StorageScope", frozen, module = "asterion_bindings._native")]
pub(crate) struct PyScope {
    pub(crate) inner: Arc<Scope>,
}
#[pymethods]
impl PyScope {
    #[new]
    fn new(database: u64, writable: Vec<u64>, readable: Vec<u64>) -> Self {
        Self {
            inner: Scope::new(database, writable, readable),
        }
    }
    fn check(&self) -> PyResult<()> {
        self.inner.check().map_err(PyValueError::new_err)
    }
    fn guarded(&self, lifetime: &Bound<'_, PyAny>) -> PyResult<Self> {
        Ok(Self {
            inner: self
                .inner
                .guarded(crate::lifetimes::one(lifetime)?)
                .map_err(PyValueError::new_err)?,
        })
    }
    fn close(&self) {
        self.inner.close();
    }
    fn initialize(&self, tables: Vec<u64>) -> PyResult<()> {
        self.inner
            .initialize(&tables)
            .map_err(PyValueError::new_err)
    }
    fn begin(&self, write: bool) -> PyResult<PyTransaction> {
        Ok(PyTransaction {
            inner: self.inner.begin(write).map_err(PyValueError::new_err)?,
        })
    }
    fn join(&self, parent: &PyTransaction, write: bool) -> PyResult<PyTransaction> {
        Ok(PyTransaction {
            inner: self
                .inner
                .join(&parent.inner, write)
                .map_err(PyValueError::new_err)?,
        })
    }
}
#[pyclass(
    name = "StorageTransaction",
    frozen,
    module = "asterion_bindings._native"
)]
pub(crate) struct PyTransaction {
    pub(crate) inner: Arc<Transaction>,
}
#[pymethods]
impl PyTransaction {
    fn check_bound(&self) -> PyResult<()> {
        self.inner.check_bound().map_err(PyValueError::new_err)
    }
    fn bind(&self, py: Python<'_>, connection: &crate::database::NativeConnection) -> PyResult<()> {
        py.detach(|| {
            connection.with_connection(|connection| {
                let ticket = connection.transaction().map_err(crate::database::error)?;
                self.inner.bind(ticket).map_err(PyValueError::new_err)
            })
        })
    }
    fn check(&self) -> PyResult<()> {
        self.inner.check().map_err(PyValueError::new_err)
    }
    fn writable(&self) -> PyResult<bool> {
        self.inner.writable().map_err(PyValueError::new_err)
    }
    fn require_write(&self) -> PyResult<()> {
        self.inner.require_write().map_err(PyValueError::new_err)
    }
    fn commit_ready(&self) -> PyResult<()> {
        self.inner.commit_ready().map_err(PyValueError::new_err)
    }
    fn abort(&self) {
        self.inner.abort();
    }
    fn close(&self) {
        self.inner.close();
    }
    fn authorize(&self, statement: &str) -> PyResult<()> {
        self.inner
            .authorize(&value(statement)?)
            .map_err(PyValueError::new_err)
    }
}
#[pyfunction]
fn storage_preflight(expected: &str, actual: &str) -> PyResult<()> {
    storage::preflight(
        &value::<Vec<storage::Schema>>(expected)?,
        &value::<Vec<storage::Schema>>(actual)?,
    )
    .map_err(PyValueError::new_err)
}
#[pyfunction]
fn storage_ownership(core: Vec<String>, owners: Vec<Vec<String>>) -> PyResult<()> {
    storage::ownership(&core, &owners).map_err(PyValueError::new_err)
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyScope>()?;
    module.add_class::<PyTransaction>()?;
    module.add_function(wrap_pyfunction!(storage_preflight, module)?)?;
    module.add_function(wrap_pyfunction!(storage_ownership, module)?)?;
    Ok(())
}
