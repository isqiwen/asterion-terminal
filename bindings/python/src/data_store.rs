//! Arrow C Data transfer and scoped ownership; scan semantics belong to L2.
use arrow_pyarrow::ToPyArrow;
use asterion_data_store::{
    DailyScan, MergeRequest, Partition, PartitionedVersion, merge, read_index, read_partition,
    verify_version,
};
use asterion_kernel::{lifetime::Lifetime, storage::Scope};
use pyo3::{exceptions::PyValueError, prelude::*, types::PyDict};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};

#[pyclass(frozen, module = "asterion_bindings._native")]
struct NativeDataScan {
    scan: Mutex<DailyScan>,
    cancelled: Arc<AtomicBool>,
    scope: Arc<Scope>,
    lifetimes: Vec<Lifetime>,
}
impl NativeDataScan {
    fn check(&self) -> Result<(), String> {
        self.scope.check()?;
        for lifetime in &self.lifetimes {
            lifetime.check()?;
        }
        Ok(())
    }
    fn finish(&self) {
        self.cancelled.store(true, Ordering::Release);
        self.scan.lock().expect("data scan ownership").close();
    }
    fn check_delivery(&self) -> Result<(), String> {
        self.check()?;
        if self.cancelled.load(Ordering::Acquire) {
            return Err("数据扫描已关闭".into());
        }
        Ok(())
    }
}
#[pymethods]
impl NativeDataScan {
    #[new]
    fn new(
        py: Python<'_>,
        files: &crate::files::ReadFilesHandle,
        storage: &crate::storage::PyScope,
        lifetimes: Vec<Py<PyAny>>,
        version_json: &str,
        request_json: &str,
    ) -> PyResult<Self> {
        let root = files.inner.clone();
        let scope = storage.inner.clone();
        let lifetimes = crate::lifetimes::parse(py, lifetimes)?;
        py.detach(|| -> Result<Self, String> {
            scope.check()?;
            for lifetime in &lifetimes {
                lifetime.check()?;
            }
            let cancelled = Arc::new(AtomicBool::new(false));
            let scan = DailyScan::new(root, version_json, request_json, cancelled.clone())?;
            Ok(Self {
                scan: Mutex::new(scan),
                cancelled,
                scope,
                lifetimes,
            })
        })
        .map_err(PyValueError::new_err)
    }
    fn next_batch<'py>(&self, py: Python<'py>) -> PyResult<Option<Bound<'py, PyAny>>> {
        let result = py
            .detach(|| -> Result<_, String> {
                let mut scan = self.scan.lock().expect("data scan ownership");
                if scan.metrics().closed {
                    return Ok(None);
                }
                let result = (|| {
                    self.check()?;
                    let batch = scan.next_batch()?;
                    self.check()?;
                    Ok(batch)
                })();
                if result.is_err() {
                    scan.close();
                }
                result
            })
            .map_err(PyValueError::new_err)?;
        let Some(batch) = result else {
            return Ok(None);
        };
        if let Err(error) = self.check_delivery() {
            py.detach(|| self.finish());
            return Err(PyValueError::new_err(error));
        }
        let result = batch.to_pyarrow(py).map(Some);
        if result.is_err() {
            py.detach(|| self.finish());
            return result;
        }
        if let Err(error) = self.check_delivery() {
            py.detach(|| self.finish());
            return Err(PyValueError::new_err(error));
        }
        result
    }
    fn close(&self, py: Python<'_>) {
        py.detach(|| self.finish());
    }
    fn metrics<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyDict>> {
        let stats = py.detach(|| self.scan.lock().expect("data scan ownership").metrics());
        let output = PyDict::new(py);
        output.set_item("files_opened", stats.files_opened)?;
        output.set_item("row_groups_decoded", stats.row_groups_decoded)?;
        output.set_item("rows_decoded", stats.rows_decoded)?;
        output.set_item("verified_bytes", stats.verified_bytes)?;
        output.set_item("closed", stats.closed)?;
        Ok(output)
    }
}
fn checked<T>(
    py: Python<'_>,
    lifetimes: Vec<Py<PyAny>>,
    call: impl FnOnce() -> Result<T, String> + Send,
) -> PyResult<T>
where
    T: Send,
{
    let lifetimes = crate::lifetimes::parse(py, lifetimes)?;
    py.detach(|| -> Result<T, String> {
        let check = || -> Result<(), String> {
            for lifetime in &lifetimes {
                lifetime.check()?;
            }
            Ok(())
        };
        check()?;
        let value = call()?;
        check()?;
        Ok(value)
    })
    .map_err(PyValueError::new_err)
}

/// Cumulative merge within the caller's fenced publication transaction.
#[pyfunction]
fn data_store_merge(
    py: Python<'_>,
    store: &crate::files::ArtifactStoreHandle,
    lifetimes: Vec<Py<PyAny>>,
    request_json: &str,
) -> PyResult<String> {
    checked(py, lifetimes, || {
        let request: MergeRequest =
            serde_json::from_str(request_json).map_err(|e| e.to_string())?;
        serde_json::to_string(&merge(&store.inner, &request)?).map_err(|e| e.to_string())
    })
}

#[pyfunction]
fn data_store_read_index(
    py: Python<'_>,
    store: &crate::files::ArtifactStoreHandle,
    lifetimes: Vec<Py<PyAny>>,
    version_json: &str,
) -> PyResult<String> {
    checked(py, lifetimes, || {
        let version: PartitionedVersion =
            serde_json::from_str(version_json).map_err(|e| e.to_string())?;
        serde_json::to_string(&read_index(&store.inner, &version)?).map_err(|e| e.to_string())
    })
}

#[pyfunction]
fn data_store_verify_version(
    py: Python<'_>,
    store: &crate::files::ArtifactStoreHandle,
    lifetimes: Vec<Py<PyAny>>,
    version_json: &str,
) -> PyResult<String> {
    checked(py, lifetimes, || {
        let version: PartitionedVersion =
            serde_json::from_str(version_json).map_err(|e| e.to_string())?;
        serde_json::to_string(&verify_version(&store.inner, &version)?).map_err(|e| e.to_string())
    })
}

#[pyfunction]
fn data_store_read_partition<'py>(
    py: Python<'py>,
    store: &crate::files::ArtifactStoreHandle,
    lifetimes: Vec<Py<PyAny>>,
    partition_json: &str,
) -> PyResult<Bound<'py, PyAny>> {
    let batch = checked(py, lifetimes, || {
        let part: Partition = serde_json::from_str(partition_json).map_err(|e| e.to_string())?;
        read_partition(&store.inner, &part)
    })?;
    batch.to_pyarrow(py)
}

/// Manifests of the built-in data types, as JSON.
#[pyfunction]
fn data_types() -> String {
    serde_json::to_string(&asterion_data_store::types::manifests()).expect("serializable")
}

/// Validate rows (JSON) of a built-in type; refusals raise ValueError.
#[pyfunction]
fn data_type_validate(py: Python<'_>, type_id: &str, rows: &str) -> PyResult<()> {
    let rows: Vec<serde_json::Value> = serde_json::from_str(rows).map_err(|_| {
        pyo3::exceptions::PyValueError::new_err("数据类型校验失败：字段、数值或时间关系无效")
    })?;
    py.detach(|| asterion_data_store::types::validate(type_id, &rows))
        .map_err(pyo3::exceptions::PyValueError::new_err)
}

/// Coverage of `rows` for an ISO date range.
#[pyfunction]
#[pyo3(signature = (type_id, rows, start=None, end=None))]
fn data_type_coverage(
    type_id: &str,
    rows: usize,
    start: Option<&str>,
    end: Option<&str>,
) -> PyResult<&'static str> {
    let day = |value: Option<&str>| {
        value
            .map(|text| chrono::NaiveDate::parse_from_str(text, "%Y-%m-%d"))
            .transpose()
            .map_err(|_| pyo3::exceptions::PyValueError::new_err("Invalid coverage date"))
    };
    asterion_data_store::types::coverage(type_id, rows, day(start)?, day(end)?)
        .map_err(pyo3::exceptions::PyValueError::new_err)
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<NativeDataScan>()?;
    module.add_function(wrap_pyfunction!(data_types, module)?)?;
    module.add_function(wrap_pyfunction!(data_type_validate, module)?)?;
    module.add_function(wrap_pyfunction!(data_type_coverage, module)?)?;
    module.add_function(wrap_pyfunction!(data_store_merge, module)?)?;
    module.add_function(wrap_pyfunction!(data_store_read_index, module)?)?;
    module.add_function(wrap_pyfunction!(data_store_verify_version, module)?)?;
    module.add_function(wrap_pyfunction!(data_store_read_partition, module)?)
}
