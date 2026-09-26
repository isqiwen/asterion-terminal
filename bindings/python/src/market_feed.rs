//! Quote value conversion. The L2 owner holds all cache and freshness state.
use asterion_market_feed::{QuoteBook, Result};
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::de::DeserializeOwned;
use std::sync::Mutex;

fn input<T: DeserializeOwned>(value: &str) -> Result<T> {
    if value.len() > 64 * 1024 {
        return Err("Market input exceeds its bound".into());
    }
    let value = asterion_foundation::communication::parse_json(value.as_bytes())?;
    serde_json::from_value(value).map_err(|_| "Invalid market input".into())
}
#[pyclass(name = "MarketQuotes", frozen, module = "asterion_bindings._native")]
pub struct PyQuotes {
    inner: Mutex<QuoteBook>,
}
impl PyQuotes {
    fn apply<T>(&self, action: impl FnOnce(&mut QuoteBook) -> Result<T>) -> Result<T> {
        let mut book = self
            .inner
            .lock()
            .map_err(|_| "Market quote state is unavailable")?;
        action(&mut book)
    }
}
#[pymethods]
impl PyQuotes {
    #[new]
    fn new() -> Self {
        Self {
            inner: Mutex::new(QuoteBook::default()),
        }
    }
    fn bind(&self, py: Python<'_>, revision: u64) -> PyResult<()> {
        py.detach(|| self.apply(|book| book.bind(revision)))
            .map_err(PyValueError::new_err)
    }
    fn subscriptions(&self, py: Python<'_>, value: &str) -> PyResult<()> {
        py.detach(|| self.apply(|book| book.subscriptions(input(value)?)))
            .map_err(PyValueError::new_err)
    }
    fn ingest(
        &self,
        py: Python<'_>,
        generation: u64,
        current: u64,
        ready: bool,
        value: &str,
        now: f64,
    ) -> PyResult<()> {
        py.detach(|| self.apply(|book| book.ingest(generation, current, ready, input(value)?, now)))
            .map_err(PyValueError::new_err)
    }
    fn subscription_error(
        &self,
        py: Python<'_>,
        generation: u64,
        current: u64,
        symbol: &str,
        detail: &str,
    ) -> PyResult<()> {
        py.detach(|| self.apply(|book| book.error(generation, current, symbol, detail)))
            .map_err(PyValueError::new_err)
    }
    fn snapshot(&self, py: Python<'_>, current: u64, ready: bool, now: f64) -> PyResult<String> {
        py.detach(|| {
            self.apply(|book| {
                serde_json::to_string(&book.snapshot(current, ready, now)?)
                    .map_err(|_| "Invalid market output".into())
            })
        })
        .map_err(PyValueError::new_err)
    }
    fn connection_event(
        &self,
        py: Python<'_>,
        generation: u64,
        current: u64,
        kind: &str,
    ) -> PyResult<()> {
        py.detach(|| {
            self.apply(|book| {
                book.connection_event(generation, current, kind);
                Ok(())
            })
        })
        .map_err(PyValueError::new_err)
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyQuotes>()?;
    Ok(())
}
