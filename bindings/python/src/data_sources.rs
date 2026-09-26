//! Data source connections and configurations of the L3 data service for the
//! internal Python process: runs on the caller's open transaction, and keeps
//! the runtime key inside the handle. Refusals raise `ValueError`, concurrent
//! changes `Conflict`.
use crate::database::{NativeConnection, NativeDatabaseError};
use crate::task_repository::TaskConflict;
use asterion_data::configuration::{self, ConfigurationUpdate, SourceError};
use asterion_data::connections;
use asterion_data::credentials::Credentials;
use asterion_data_store::provider::{ConfigurationSpec, ProviderManifest};
use asterion_store::Transaction;
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::Serialize;
use serde::de::DeserializeOwned;
use std::path::PathBuf;

fn error(value: SourceError) -> PyErr {
    match value {
        SourceError::Refused(message) => PyValueError::new_err(message),
        SourceError::Conflict(message) => TaskConflict::new_err(message),
        SourceError::Store(error) => NativeDatabaseError::new_err((
            if error.integrity {
                "integrity"
            } else {
                "operational"
            },
            error.message,
        )),
    }
}

fn parsed<T: DeserializeOwned>(text: &str) -> PyResult<T> {
    serde_json::from_str(text).map_err(|error| PyValueError::new_err(error.to_string()))
}

fn run<T: Serialize + Send>(
    py: Python<'_>,
    connection: &NativeConnection,
    work: impl FnOnce(&Transaction) -> Result<T, SourceError> + Send,
) -> PyResult<String> {
    py.detach(|| {
        connection.with_connection(|connection| work(&Transaction::over(connection)).map_err(error))
    })
    .map(|value| serde_json::to_string(&value).expect("serializable value"))
}

/// Configuration snapshots under a data root, sealed with the runtime key.
#[pyclass(frozen)]
pub(crate) struct DataCredentials {
    pub(crate) inner: Credentials,
}

#[pymethods]
impl DataCredentials {
    #[new]
    fn new(master: &str, root: PathBuf) -> Self {
        Self {
            inner: Credentials::new(master, &root),
        }
    }

    /// Fix an enabled connection's current configuration for a new sync task.
    fn fix_for_task(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        sources: &str,
        owner: &str,
        provider: &str,
    ) -> PyResult<String> {
        let sources: Vec<ProviderManifest> = parsed(sources)?;
        run(py, connection, |tx| {
            connections::fix_for_task(tx, &self.inner, &sources, owner, provider)
        })
    }

    /// The runnable values a task was fixed to.
    fn resolve(&self, fixed: &str, owner: &str, spec: &str) -> PyResult<String> {
        let fixed: serde_json::Value = parsed(fixed)?;
        let spec: ConfigurationSpec = parsed(spec)?;
        configuration::resolve(&self.inner, &fixed, owner, &spec)
            .map(|values| serde_json::to_string(&values).expect("serializable values"))
            .map_err(error)
    }

    fn state(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        sources: &str,
        owner: &str,
    ) -> PyResult<String> {
        let sources: Vec<ProviderManifest> = parsed(sources)?;
        run(py, connection, |tx| {
            let spec = connections::spec(tx, &sources, owner)?;
            configuration::state(tx, &self.inner, owner, &spec)
        })
    }

    fn apply(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        sources: &str,
        owner: &str,
        update: &str,
    ) -> PyResult<String> {
        let sources: Vec<ProviderManifest> = parsed(sources)?;
        let update: ConfigurationUpdate = parsed(update)?;
        if !update.valid() {
            return Err(PyValueError::new_err("Invalid configuration update"));
        }
        run(py, connection, |tx| {
            let spec = connections::spec(tx, &sources, owner)?;
            configuration::apply(tx, &self.inner, owner, &spec, &update)
        })
    }

    fn listing(
        &self,
        py: Python<'_>,
        connection: &NativeConnection,
        sources: &str,
    ) -> PyResult<String> {
        let sources: Vec<ProviderManifest> = parsed(sources)?;
        run(py, connection, |tx| {
            connections::listing(tx, &self.inner, &sources)
        })
    }

    /// Whether sealed bytes open with this runtime key (backup checks).
    fn opens(&self, sealed: &[u8]) -> bool {
        self.inner.opens(sealed)
    }
}

#[pyfunction]
fn data_connection_state(
    py: Python<'_>,
    connection: &NativeConnection,
    sources: &str,
    identifier: &str,
) -> PyResult<String> {
    let sources: Vec<ProviderManifest> = parsed(sources)?;
    run(py, connection, |tx| {
        connections::state(tx, &sources, identifier)
    })
}

#[pyfunction]
fn data_connection_create(
    py: Python<'_>,
    connection: &NativeConnection,
    sources: &str,
    body: &str,
) -> PyResult<String> {
    let sources: Vec<ProviderManifest> = parsed(sources)?;
    let body: connections::NewConnection = parsed(body)?;
    if !(1..=80).contains(&body.name.chars().count()) {
        return Err(PyValueError::new_err("Invalid connection name"));
    }
    run(py, connection, |tx| {
        connections::create(tx, &sources, &body)
    })
}

#[pyfunction]
fn data_connection_update(
    py: Python<'_>,
    connection: &NativeConnection,
    sources: &str,
    identifier: &str,
    body: &str,
) -> PyResult<String> {
    let sources: Vec<ProviderManifest> = parsed(sources)?;
    let body: connections::ConnectionUpdate = parsed(body)?;
    if !body.valid() {
        return Err(PyValueError::new_err("Invalid connection update"));
    }
    run(py, connection, |tx| {
        connections::update(tx, &sources, identifier, &body)
    })
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<DataCredentials>()?;
    m.add_function(wrap_pyfunction!(data_connection_state, m)?)?;
    m.add_function(wrap_pyfunction!(data_connection_create, m)?)?;
    m.add_function(wrap_pyfunction!(data_connection_update, m)?)?;
    Ok(())
}
