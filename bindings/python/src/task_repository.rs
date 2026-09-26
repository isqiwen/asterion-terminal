//! Borrow native transaction handles; task state and persistence remain in L1.
use crate::{database::NativeConnection, storage::PyTransaction};
use asterion_kernel::{
    database::OperationError,
    tasks::repository::{Grant, Repository, Request},
};
use pyo3::{exceptions::PyValueError, prelude::*};

pyo3::create_exception!(_native, TaskConflict, PyValueError);
fn input<T: serde::de::DeserializeOwned>(value: &str) -> PyResult<T> {
    if value.len() > 64 * 1024 * 1024 {
        return Err(TaskConflict::new_err("Task operation exceeds input budget"));
    }
    let value = asterion_foundation::communication::parse_json(value.as_bytes())
        .map_err(|_| TaskConflict::new_err("Invalid task operation"))?;
    serde_json::from_value(value).map_err(|_| TaskConflict::new_err("Invalid task operation"))
}
fn error(value: OperationError) -> PyErr {
    match value {
        OperationError::Database(error) => crate::database::error(error),
        OperationError::Contract(message) => TaskConflict::new_err(message),
    }
}
fn output(value: serde_json::Value) -> PyResult<String> {
    serde_json::to_string(&value).map_err(|_| TaskConflict::new_err("Invalid task output"))
}

#[pyclass(name = "TaskRepository", frozen, module = "asterion_bindings._native")]
struct PyRepository {
    inner: Repository,
}
#[pymethods]
impl PyRepository {
    #[new]
    fn new(database: u64, topic: &str) -> PyResult<Self> {
        Ok(Self {
            inner: Repository::new(database, input(topic)?).map_err(error)?,
        })
    }
    fn run(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        request: &str,
        trace: &str,
    ) -> PyResult<String> {
        let request: Request = input(request)?;
        let trace = input(trace)?;
        output(py.detach(|| {
            connection.with_connection(|connection| {
                self.inner.run(connection, request, trace).map_err(error)
            })
        })?)
    }
    fn granted(&self, kinds: Vec<String>) -> PyGrant {
        PyGrant {
            inner: self.inner.granted(kinds.into_iter().collect()),
        }
    }
}
#[pyclass(name = "TaskGrant", frozen, module = "asterion_bindings._native")]
struct PyGrant {
    inner: Grant,
}
#[pymethods]
impl PyGrant {
    fn submit(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        record: &str,
        trace: &str,
    ) -> PyResult<String> {
        let record = input(record)?;
        let trace = input(trace)?;
        output(py.detach(|| {
            connection.with_connection(|connection| {
                self.inner.submit(connection, record, trace).map_err(error)
            })
        })?)
    }
    fn run(
        &self,
        py: Python<'_>,
        transaction: &PyTransaction,
        request: &str,
        trace: &str,
    ) -> PyResult<String> {
        let request = input(request)?;
        let trace = input(trace)?;
        output(py.detach(|| {
            self.inner
                .run(&transaction.inner, request, trace)
                .map_err(error)
        })?)
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add("TaskConflict", module.py().get_type::<TaskConflict>())?;
    module.add_class::<PyRepository>()?;
    module.add_class::<PyGrant>()?;
    Ok(())
}
