//! Python object conversion only. The L2 manager owns every lifecycle decision.
use asterion_connections as domain;
use pyo3::{
    exceptions::{PyOSError, PyValueError},
    prelude::*,
    types::PyBytes,
};
use serde::{Serialize, de::DeserializeOwned};
use serde_json::Value;
use std::sync::Arc;

pyo3::create_exception!(_native, ConnectionsError, PyValueError);
fn error(error: domain::Error) -> PyErr {
    if error.io {
        return PyOSError::new_err(error.message);
    }
    Python::attach(|py| {
        let result = ConnectionsError::new_err(error.message);
        let _ = result.value(py).setattr("category", error.category);
        let _ = result.value(py).setattr("retryable", error.retryable);
        result
    })
}
fn parse<T: DeserializeOwned>(value: &str) -> PyResult<T> {
    if value.len() > domain::MAX_STATE_BYTES {
        return Err(PyValueError::new_err("Connection contract exceeds budget"));
    }
    let value = asterion_foundation::communication::parse_json(value.as_bytes())
        .map_err(|_| PyValueError::new_err("Invalid connection contract"))?;
    serde_json::from_value(value).map_err(|_| PyValueError::new_err("Invalid connection contract"))
}
fn encoded(value: impl Serialize) -> PyResult<String> {
    serde_json::to_string(&value).map_err(|_| PyValueError::new_err("Invalid connection value"))
}
fn serialize(value: impl Serialize) -> domain::Result<String> {
    serde_json::to_string(&value).map_err(Into::into)
}
fn source_error() -> domain::Error {
    domain::Error::source("来源查询失败或响应未通过校验", "incomplete", true)
}
fn callback_error(error: PyErr, py: Python<'_>) -> domain::Error {
    // Only the explicitly structured connector error crosses the source boundary.
    let value = error.value(py);
    let category = value
        .getattr("category")
        .and_then(|v| v.extract::<String>());
    let retryable = value.getattr("retryable").and_then(|v| v.extract::<bool>());
    match (category, retryable) {
        (Ok(category), Ok(retryable)) => {
            let category = match category.as_str() {
                "unavailable" => "unavailable",
                "unsupported" => "unsupported",
                "session_invalid" => "session_invalid",
                "incomplete" => "incomplete",
                _ => return source_error(),
            };
            domain::Error::source(
                value.str().map(|s| s.to_string()).unwrap_or_default(),
                category,
                retryable,
            )
        }
        _ => source_error(),
    }
}
fn completed(value: &Bound<'_, PyAny>) -> PyResult<()> {
    if value.is_none() {
        return Ok(());
    }
    let inspect = value.py().import("inspect")?;
    if inspect.call_method1("iscoroutine", (value,))?.is_truthy()? {
        value.call_method0("close")?;
    }
    Err(PyValueError::new_err(
        "Connection lifecycle callbacks must complete synchronously and return None",
    ))
}

struct Secrets(Py<PyAny>);
impl domain::SecretPort for Secrets {
    fn encrypt(&self, value: &[u8]) -> domain::Result<Vec<u8>> {
        self.call("encrypt", value)
    }
    fn decrypt(&self, value: &[u8]) -> domain::Result<Vec<u8>> {
        self.call("decrypt", value)
    }
}
impl Secrets {
    fn call(&self, method: &str, value: &[u8]) -> domain::Result<Vec<u8>> {
        Python::attach(|py| {
            self.0
                .call_method1(py, method, (PyBytes::new(py, value),))?
                .extract(py)
        })
        .map_err(|_: PyErr| domain::Error::invalid("连接凭据校验失败"))
    }
}
struct Source {
    object: Py<PyAny>,
    dispatch: Py<PyAny>,
}
impl domain::Connector for Source {
    fn validate(&self, config: &domain::Values, secrets: &domain::Values) -> domain::Result<()> {
        let (config, secrets) = (serialize(config)?, serialize(secrets)?);
        Python::attach(|py| {
            let value = self
                .object
                .call_method1(py, "validate", (config, secrets))
                .map_err(|error| {
                    if error.is_instance_of::<PyValueError>(py) {
                        domain::Error::invalid(
                            error
                                .value(py)
                                .str()
                                .map(|s| s.to_string())
                                .unwrap_or_default(),
                        )
                    } else {
                        domain::Error::invalid("连接配置未通过来源校验")
                    }
                })?;
            completed(value.bind(py)).map_err(|_| domain::Error::invalid("接入校验必须同步完成"))
        })
    }
    fn open(
        &self,
        profile: &domain::ConnectionProfile,
        secrets: &domain::Values,
    ) -> domain::Result<Arc<dyn domain::Session>> {
        let (profile, secrets) = (serialize(profile)?, serialize(secrets)?);
        Python::attach(|py| {
            let object = self
                .object
                .call_method1(py, "open", (profile, secrets))
                .map_err(|_| source_error())?;
            Ok(Arc::new(Session {
                object,
                dispatch: self.dispatch.clone_ref(py),
            }) as Arc<dyn domain::Session>)
        })
    }
}
struct Session {
    object: Py<PyAny>,
    dispatch: Py<PyAny>,
}
impl Session {
    fn no_args(&self, method: &str) -> domain::Result<()> {
        Python::attach(|py| {
            let value = self.object.call_method0(py, method)?;
            completed(value.bind(py))
        })
        .map_err(|_: PyErr| source_error())
    }
}
impl domain::Session for Session {
    fn start_market(
        &self,
        subscriptions: &[domain::Subscription],
        emit: domain::Emitter,
    ) -> domain::Result<()> {
        let subscriptions = serialize(subscriptions)?;
        Python::attach(|py| {
            let emit = PyEmitter {
                inner: emit,
                dispatch: self.dispatch.clone_ref(py),
            };
            let value = self.object.call_method1(
                py,
                "start_market",
                (subscriptions, Py::new(py, emit)?),
            )?;
            completed(value.bind(py))
        })
        .map_err(|_: PyErr| source_error())
    }
    fn subscriptions(&self, subscriptions: &[domain::Subscription]) -> domain::Result<()> {
        let subscriptions = serialize(subscriptions)?;
        Python::attach(|py| {
            let value = self
                .object
                .call_method1(py, "subscriptions", (subscriptions,))?;
            completed(value.bind(py))
        })
        .map_err(|_: PyErr| source_error())
    }
    fn stop_market(&self) -> domain::Result<()> {
        self.no_args("stop_market")
    }
    fn close(&self) -> domain::Result<()> {
        self.no_args("close")
    }
    fn read(
        &self,
        kind: domain::ReadKind,
        request: &domain::ReadRequest,
        cancel: Arc<domain::ReadTicket>,
    ) -> domain::Result<Value> {
        let request = serialize(request)?;
        Python::attach(|py| {
            let result = self
                .object
                .call_method1(
                    py,
                    "read",
                    (
                        match kind {
                            domain::ReadKind::Instruments => "instruments",
                            domain::ReadKind::Account => "account",
                        },
                        request,
                        Py::new(py, PyCancellation { inner: cancel })
                            .map_err(|_| source_error())?,
                    ),
                )
                .map_err(|e| callback_error(e, py))?;
            let raw: String = result.extract(py).map_err(|_| source_error())?;
            if raw.len() > domain::MAX_READ_BYTES {
                return Err(source_error());
            }
            asterion_foundation::communication::parse_json(raw.as_bytes())
                .map_err(|_| source_error())
        })
    }
}
struct Caller(Py<PyAny>);
impl domain::Cancellation for Caller {
    fn is_cancelled(&self) -> bool {
        Python::attach(|py| {
            self.0
                .call_method0(py, "is_set")
                .and_then(|v| v.extract(py))
                .unwrap_or(true)
        })
    }
}
#[pyclass(
    name = "ConnectionReadCancellation",
    frozen,
    module = "asterion_bindings._native"
)]
struct PyCancellation {
    inner: Arc<domain::ReadTicket>,
}
#[pymethods]
impl PyCancellation {
    fn is_set(&self, py: Python<'_>) -> bool {
        py.detach(|| self.inner.is_cancelled())
    }
}
#[pyclass(
    name = "ConnectionEmitter",
    frozen,
    module = "asterion_bindings._native"
)]
struct PyEmitter {
    inner: domain::Emitter,
    dispatch: Py<PyAny>,
}
#[pymethods]
impl PyEmitter {
    fn __call__(&self, py: Python<'_>, kind: &str, value: Py<PyAny>) -> PyResult<()> {
        let detail = value.extract::<String>(py).ok();
        if let Some(generation) = self.inner.accept(kind, detail.as_deref()) {
            let returned = self.inner.dispatch(|| {
                self.dispatch
                    .call1(py, (self.inner.connection_id(), generation, kind, value))
            })?;
            completed(returned.bind(py))?;
        }
        Ok(())
    }
}
#[pyclass(name = "Connections", frozen, module = "asterion_bindings._native")]
struct PyConnections {
    inner: Arc<domain::Manager>,
    dispatch: Py<PyAny>,
}
#[pymethods]
impl PyConnections {
    #[new]
    fn new(
        py: Python<'_>,
        path: String,
        secrets: Py<PyAny>,
        owners: &str,
        dispatch: Py<PyAny>,
    ) -> PyResult<Self> {
        let owners = parse(owners)?;
        let inner = py.detach(|| domain::Manager::new(path, Arc::new(Secrets(secrets)), owners));
        Ok(Self { inner, dispatch })
    }
    fn initialize(
        &self,
        py: Python<'_>,
        descriptors: &str,
        sources: Vec<Py<PyAny>>,
    ) -> PyResult<()> {
        let descriptors: Vec<domain::ConnectorDescriptor> = parse(descriptors)?;
        if descriptors.len() != sources.len() {
            return Err(PyValueError::new_err(
                "Connector declarations and sources differ",
            ));
        }
        let catalog = descriptors
            .into_iter()
            .zip(sources)
            .map(|(descriptor, object)| domain::Contribution {
                descriptor,
                source: Arc::new(Source {
                    object,
                    dispatch: self.dispatch.clone_ref(py),
                }),
            })
            .collect();
        py.detach(|| self.inner.initialize(catalog)).map_err(error)
    }
    fn profiles(&self) -> Vec<String> {
        self.inner.profiles()
    }
    fn profile(&self, id: &str) -> PyResult<String> {
        encoded(self.inner.profile(id).map_err(error)?)
    }
    fn channel(&self, id: &str, channel: &str) -> PyResult<String> {
        encoded(
            self.inner
                .channel(id, parse(&encoded(channel)?)?)
                .map_err(error)?,
        )
    }
    fn supports(&self, id: &str, feature: &str) -> PyResult<bool> {
        Ok(self.inner.supports(id, parse(&encoded(feature)?)?))
    }
    fn active_id(&self) -> Option<String> {
        self.inner.active_id()
    }
    fn view(&self, py: Python<'_>, id: &str) -> PyResult<String> {
        encoded(py.detach(|| self.inner.view(id)).map_err(error)?)
    }
    fn snapshot(&self, py: Python<'_>) -> PyResult<String> {
        encoded(py.detach(|| self.inner.snapshot()).map_err(error)?)
    }
    fn save(&self, py: Python<'_>, body: &str) -> PyResult<String> {
        let body = parse(body)?;
        encoded(py.detach(|| self.inner.save(body)).map_err(error)?)
    }
    fn delete(&self, py: Python<'_>, id: &str, revision: u64) -> PyResult<String> {
        encoded(
            py.detach(|| self.inner.delete(id, revision))
                .map_err(error)?,
        )
    }
    fn select(&self, py: Python<'_>, id: &str) -> PyResult<String> {
        encoded(py.detach(|| self.inner.select(id)).map_err(error)?)
    }
    fn connect(&self, py: Python<'_>, id: &str) -> PyResult<String> {
        encoded(py.detach(|| self.inner.connect(id)).map_err(error)?)
    }
    fn disconnect(&self, py: Python<'_>, id: &str) -> PyResult<String> {
        encoded(py.detach(|| self.inner.disconnect(id)).map_err(error)?)
    }
    fn subscribe(&self, py: Python<'_>, id: &str, subscriptions: &str) -> PyResult<()> {
        let subscriptions: Vec<domain::Subscription> = parse(subscriptions)?;
        py.detach(|| self.inner.subscribe(id, subscriptions))
            .map_err(error)
    }
    fn read(&self, py: Python<'_>, id: &str, kind: &str, cancel: Py<PyAny>) -> PyResult<String> {
        let kind = parse(&encoded(kind)?)?;
        encoded(
            py.detach(|| self.inner.read(id, kind, Arc::new(Caller(cancel))))
                .map_err(error)?,
        )
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| self.inner.close()).map_err(error)
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyConnections>()?;
    module.add_class::<PyEmitter>()?;
    module.add_class::<PyCancellation>()?;
    module.add(
        "ConnectionsError",
        module.py().get_type::<ConnectionsError>(),
    )?;
    Ok(())
}
