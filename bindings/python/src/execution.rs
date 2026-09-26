//! Ownership and JSON conversion for the fixed daily execution capability.
use asterion_execution::{DailyAccount, DailyBar, ExecutionConfig, ExecutionFactory};
use asterion_foundation::communication::parse_json;
use asterion_kernel::lifetime::Lifetime;
use pyo3::{exceptions::PyValueError, prelude::*};
use std::sync::{Mutex, MutexGuard};

fn error(message: impl ToString) -> PyErr {
    PyValueError::new_err(message.to_string())
}
fn check(scopes: &[Lifetime]) -> Result<(), String> {
    for scope in scopes {
        scope.check()?;
    }
    Ok(())
}
fn lock<T>(value: &Mutex<T>) -> PyResult<MutexGuard<'_, T>> {
    value
        .try_lock()
        .map_err(|_| error("Execution session is busy or unavailable"))
}
fn parse<T: serde::de::DeserializeOwned>(input: &str, budget: usize) -> Result<T, String> {
    if input.len() > budget {
        return Err("Execution input exceeds byte budget".into());
    }
    serde_json::from_value(parse_json(input.as_bytes())?).map_err(|e| e.to_string())
}
#[pyclass(name = "ExecutionFactory")]
pub struct PyFactory {
    owner: ExecutionFactory,
    scopes: Vec<Lifetime>,
}
#[pymethods]
impl PyFactory {
    #[new]
    fn new(py: Python<'_>, lifetimes: Vec<Py<PyAny>>) -> PyResult<Self> {
        Ok(Self {
            owner: ExecutionFactory::default(),
            scopes: crate::lifetimes::parse(py, lifetimes)?,
        })
    }
    fn create(
        &self,
        py: Python<'_>,
        config: String,
        lifetimes: Vec<Py<PyAny>>,
    ) -> PyResult<PyAccount> {
        let mut scopes = self.scopes.clone();
        scopes.extend(crate::lifetimes::parse(py, lifetimes)?);
        if scopes.len() > 128 {
            return Err(error("Resource lifetime nesting exceeds its budget"));
        }
        let account = py
            .detach(|| {
                check(&scopes)?;
                let account = self
                    .owner
                    .open(parse::<ExecutionConfig>(&config, 16 * 1024 * 1024)?)?;
                check(&scopes)?;
                account.check()?;
                Ok::<_, String>(account)
            })
            .map_err(error)?;
        Ok(PyAccount {
            account: Mutex::new(Some(account)),
            scopes,
        })
    }
    fn close(&self) {
        self.owner.close();
    }
}
#[pyclass(name = "DailyAccount")]
pub struct PyAccount {
    account: Mutex<Option<DailyAccount>>,
    scopes: Vec<Lifetime>,
}
#[pymethods]
impl PyAccount {
    fn begin_day(&self, py: Python<'_>, bar: String) -> PyResult<String> {
        py.detach(|| {
            let mut slot = self
                .account
                .try_lock()
                .map_err(|_| "Execution session is busy or unavailable".to_string())?;
            let result = (|| {
                check(&self.scopes)?;
                let account = slot.as_mut().ok_or("Account session is closed")?;
                account.check()?;
                if bar.len() > 64 * 1024 {
                    return Err("Execution bar exceeds byte budget".into());
                }
                let value = parse_json(bar.as_bytes())?;
                asterion_execution::settlement_price(
                    value.get("settle").cloned().unwrap_or_default(),
                )?;
                let bar: DailyBar = serde_json::from_value(value).map_err(|e| e.to_string())?;
                let result = account.begin_day(bar)?;
                check(&self.scopes)?;
                account.check()?;
                serde_json::to_string(&result).map_err(|e| e.to_string())
            })();
            if result.is_err() {
                slot.take();
            }
            result
        })
        .map_err(error)
    }
    fn close_intent(&self, py: Python<'_>, long: bool) -> PyResult<()> {
        py.detach(|| {
            let mut slot = self
                .account
                .try_lock()
                .map_err(|_| "Execution session is busy or unavailable".to_string())?;
            let result = (|| {
                check(&self.scopes)?;
                let account = slot.as_mut().ok_or("Account session is closed")?;
                account.close_intent(long)?;
                check(&self.scopes)?;
                account.check()
            })();
            if result.is_err() {
                slot.take();
            }
            result
        })
        .map_err(error)
    }
    fn finish(&self, py: Python<'_>) -> PyResult<String> {
        py.detach(|| {
            let mut slot = self
                .account
                .try_lock()
                .map_err(|_| "Execution session is busy or unavailable".to_string())?;
            let account = slot.take().ok_or("Account session is closed")?;
            check(&self.scopes)?;
            let result = account.finish()?;
            check(&self.scopes)?;
            serde_json::to_string(&result).map_err(|e| e.to_string())
        })
        .map_err(error)
    }
    fn close(&self) -> PyResult<()> {
        lock(&self.account)?.take();
        Ok(())
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyFactory>()?;
    module.add_class::<PyAccount>()?;
    Ok(())
}
