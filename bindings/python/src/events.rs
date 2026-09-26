//! Object ownership and JSON conversion for the fixed native event repository.
use crate::{database::NativeConnection, storage::PyTransaction};
use asterion_kernel::{
    database::OperationError,
    events::{self, Journal, ReadPlan, Topic, Writer},
};
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::{Serialize, de::DeserializeOwned};

fn input<T: DeserializeOwned>(raw: &str) -> PyResult<T> {
    let value = asterion_foundation::communication::parse_json(raw.as_bytes())
        .map_err(PyValueError::new_err)?;
    serde_json::from_value(value).map_err(|error| PyValueError::new_err(error.to_string()))
}
fn output(value: impl Serialize) -> PyResult<String> {
    serde_json::to_string(&value).map_err(|error| PyValueError::new_err(error.to_string()))
}
fn error(value: OperationError) -> PyErr {
    match value {
        OperationError::Database(error) => crate::database::error(error),
        OperationError::Contract(message) => PyValueError::new_err(message),
    }
}
#[pyfunction]
fn event_topic_validate(value: &str) -> PyResult<()> {
    input::<Topic>(value)?
        .validate()
        .map_err(PyValueError::new_err)
}
#[pyclass(name = "EventRegistry", frozen, module = "asterion_bindings._native")]
struct PyRegistry {
    inner: Journal,
}
#[pymethods]
impl PyRegistry {
    #[new]
    fn new(database: u64, topics: &str) -> PyResult<Self> {
        Ok(Self {
            inner: Journal::new(database, input(topics)?).map_err(PyValueError::new_err)?,
        })
    }
    fn check_topic(&self, topic: &str) -> PyResult<()> {
        self.inner
            .registry()
            .topic(&input(topic)?)
            .map_err(PyValueError::new_err)
    }
    fn read_path(&self, topic: &str, method: &str) -> PyResult<String> {
        self.inner
            .registry()
            .read_path(topic, method)
            .map(str::to_owned)
            .map_err(PyValueError::new_err)
    }
    fn publisher(&self, owner: &str, topics: &str) -> PyResult<PyPublisher> {
        Ok(PyPublisher {
            inner: self
                .inner
                .publisher(owner, &input::<Vec<Topic>>(topics)?)
                .map_err(PyValueError::new_err)?,
        })
    }
    fn publish_host(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        topic: &str,
        stream: String,
        payload: &str,
        trace: &str,
    ) -> PyResult<String> {
        let topic = input(topic)?;
        let payload = input(payload)?;
        let trace = input(trace)?;
        let message = py.detach(|| {
            connection.with_connection(|connection| {
                self.inner
                    .publish_host(connection, topic, stream, payload, trace)
                    .map_err(error)
            })
        })?;
        output(message)
    }
    fn read_plan(&self, request: &str) -> PyResult<String> {
        #[derive(serde::Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Request {
            topic: String,
            after: String,
            limit: i64,
        }
        let request: Request = input(request)?;
        output(
            self.inner
                .registry()
                .read_plan(&request.topic, &request.after, request.limit)
                .map_err(PyValueError::new_err)?,
        )
    }
    fn read(&self, py: Python<'_>, connection: &NativeConnection, plan: &str) -> PyResult<String> {
        let plan: ReadPlan = input(plan)?;
        let page = py.detach(|| {
            connection
                .with_connection(|connection| self.inner.read(connection, plan).map_err(error))
        })?;
        output(page)
    }
}
#[pyclass(name = "EventPublisher", frozen, module = "asterion_bindings._native")]
struct PyPublisher {
    inner: Writer,
}
#[pymethods]
impl PyPublisher {
    fn check(&self, transaction: &PyTransaction, topic: &str) -> PyResult<()> {
        self.inner
            .check(&transaction.inner, &input(topic)?)
            .map_err(error)
    }
    fn guarded(&self, handle: &crate::host::PyHandle) -> PyResult<Self> {
        Ok(Self {
            inner: self
                .inner
                .guarded(handle.handle.clone())
                .map_err(PyValueError::new_err)?,
        })
    }
    fn publish(
        &self,
        py: Python<'_>,
        transaction: &PyTransaction,
        topic: &str,
        stream: String,
        payload: &str,
        trace: &str,
    ) -> PyResult<String> {
        let topic = input(topic)?;
        let payload = input(payload)?;
        let trace = input(trace)?;
        let message = py.detach(|| {
            self.inner
                .publish(&transaction.inner, topic, stream, payload, trace)
                .map_err(error)
        })?;
        output(message)
    }
}
#[pyfunction]
fn event_journal_validate(py: Python<'_>, connection: &NativeConnection) -> PyResult<u64> {
    py.detach(|| {
        connection.with_connection(|connection| events::validate_journal(connection).map_err(error))
    })
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_function(wrap_pyfunction!(event_topic_validate, module)?)?;
    module.add_function(wrap_pyfunction!(event_journal_validate, module)?)?;
    module.add_class::<PyRegistry>()?;
    module.add_class::<PyPublisher>()?;
    Ok(())
}
