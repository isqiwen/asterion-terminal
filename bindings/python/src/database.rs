//! DBAPI ownership conversion. All physical I/O and resource policy stay in L1.
use asterion_kernel::database::{self, Cursor, Database, Lease};
use pyo3::{exceptions::PyException, prelude::*};
use std::sync::Mutex;

pyo3::create_exception!(_native, NativeDatabaseError, PyException);
pub(crate) fn error(value: database::Error) -> PyErr {
    NativeDatabaseError::new_err((value.code, value.message))
}
fn closed() -> PyErr {
    NativeDatabaseError::new_err(("closed", "Database connection is closed"))
}
fn invalid() -> PyErr {
    NativeDatabaseError::new_err(("programming", "Invalid database request"))
}
#[pyclass(frozen, module = "asterion_bindings._native")]
pub(crate) struct NativeConnection {
    lease: Mutex<Lease>,
}
impl NativeConnection {
    pub(crate) fn with_connection<T>(
        &self,
        action: impl FnOnce(&database::Connection) -> PyResult<T>,
    ) -> PyResult<T> {
        let lease = self.lease.lock().map_err(|_| closed())?;
        action(lease.connection().map_err(error)?)
    }
}
#[pymethods]
impl NativeConnection {
    #[new]
    fn new(py: Python<'_>, config: &str) -> PyResult<Self> {
        Ok(Self {
            lease: Mutex::new(py.detach(|| Lease::open(config)).map_err(error)?),
        })
    }
    fn begin_write(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| self.with_connection(|connection| connection.begin_write().map_err(error)))
    }
    fn begin(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .begin()
                .map_err(error)
        })
    }
    fn commit(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .commit()
                .map_err(error)
        })
    }
    fn rollback(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .rollback()
                .map_err(error)
        })
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .close()
                .map_err(error)
        })
    }
    #[getter]
    fn autocommit(&self, py: Python<'_>) -> PyResult<bool> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .autocommit()
                .map_err(error)
        })
    }
    #[setter]
    fn set_autocommit(&self, py: Python<'_>, value: bool) -> PyResult<()> {
        py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .set_autocommit(value)
                .map_err(error)
        })
    }
    #[pyo3(signature = (sql, params, stream=false))]
    fn execute(
        &self,
        py: Python<'_>,
        sql: &str,
        params: &str,
        stream: bool,
    ) -> PyResult<NativeCursor> {
        if params.len() > 64 * 1024 * 1024 {
            return Err(invalid());
        }
        let params = serde_json::from_str(params).map_err(|_| invalid())?;
        let inner = py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .execute(sql, params, stream)
                .map_err(error)
        })?;
        Ok(NativeCursor { inner })
    }
    fn executemany(&self, py: Python<'_>, sql: &str, params: &str) -> PyResult<NativeCursor> {
        if params.len() > 64 * 1024 * 1024 {
            return Err(invalid());
        }
        let params = serde_json::from_str(params).map_err(|_| invalid())?;
        let inner = py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .executemany(sql, params)
                .map_err(error)
        })?;
        Ok(NativeCursor { inner })
    }
    fn table_names(&self, py: Python<'_>) -> PyResult<String> {
        let names = py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .table_names()
                .map_err(error)
        })?;
        serde_json::to_string(&names).map_err(|_| invalid())
    }
    fn observe_schema(&self, py: Python<'_>) -> PyResult<String> {
        let schema = py.detach(|| {
            self.lease
                .lock()
                .map_err(|_| closed())?
                .connection()
                .map_err(error)?
                .observe_schema()
                .map_err(error)
        })?;
        serde_json::to_string(&schema).map_err(|_| invalid())
    }
}
#[pyclass(frozen, module = "asterion_bindings._native")]
struct NativeCursor {
    inner: Cursor,
}
#[pymethods]
impl NativeCursor {
    fn guard(&self, py: Python<'_>, transaction: &crate::storage::PyTransaction) -> PyResult<()> {
        py.detach(|| self.inner.guard(transaction.inner.clone()))
            .map_err(error)
    }
    fn description(&self) -> PyResult<String> {
        let values: Vec<_> = self
            .inner
            .description
            .iter()
            .map(|column| (&column.name, &column.kind))
            .collect();
        serde_json::to_string(&values).map_err(|_| invalid())
    }
    #[getter]
    fn rowcount(&self) -> i64 {
        self.inner.rowcount
    }
    #[getter]
    fn lastrowid(&self) -> Option<i64> {
        self.inner.lastrowid
    }
    fn fetchmany(&self, py: Python<'_>, size: usize) -> PyResult<String> {
        let rows = py.detach(|| self.inner.fetchmany(size)).map_err(error)?;
        serde_json::to_string(&rows).map_err(|_| invalid())
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| self.inner.close()).map_err(error)
    }
}
#[pyclass(frozen, module = "asterion_bindings._native")]
struct NativeDatabase {
    inner: Database,
}
#[pymethods]
impl NativeDatabase {
    #[new]
    fn new(config: String, pool_size: usize, wait_seconds: f64) -> PyResult<Self> {
        Ok(Self {
            inner: Database::new(config, pool_size, wait_seconds).map_err(error)?,
        })
    }
    fn identity(&self) -> u64 {
        self.inner.id()
    }
    fn connect(&self, py: Python<'_>) -> PyResult<NativeConnection> {
        Ok(NativeConnection {
            lease: Mutex::new(py.detach(|| self.inner.connect()).map_err(error)?),
        })
    }
    fn dispose(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| self.inner.dispose()).map_err(error)
    }
}
#[pyfunction]
fn database_sqlite_version() -> (u32, u32, u32) {
    database::sqlite_version()
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add(
        "NativeDatabaseError",
        module.py().get_type::<NativeDatabaseError>(),
    )?;
    module.add_class::<NativeConnection>()?;
    module.add_class::<NativeCursor>()?;
    module.add_class::<NativeDatabase>()?;
    module.add_function(wrap_pyfunction!(database_sqlite_version, module)?)?;
    Ok(())
}

impl Drop for NativeConnection {
    fn drop(&mut self) {
        // Python GC can release a lease without an explicit close. Rollback may
        // wait on I/O, so it must not hold the interpreter lock either.
        let _ = Python::try_attach(|py| {
            py.detach(|| {
                if let Ok(lease) = self.lease.get_mut() {
                    let _ = lease.close();
                }
            });
        });
    }
}
impl Drop for NativeCursor {
    fn drop(&mut self) {
        let _ = Python::try_attach(|py| {
            py.detach(|| {
                let _ = self.inner.close();
            });
        });
    }
}
impl Drop for NativeDatabase {
    fn drop(&mut self) {
        let _ = Python::try_attach(|py| {
            py.detach(|| {
                let _ = self.inner.dispose();
            });
        });
    }
}
