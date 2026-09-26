//! Python ownership and byte conversion for the fixed L1 filesystem mechanisms.
use asterion_kernel::artifacts::{ArtifactRef, ArtifactStore};
use asterion_kernel::files::{self, FileLock, ReadRoot};
use pyo3::{
    exceptions::{PyOSError, PyValueError},
    prelude::*,
    types::PyBytes,
};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pyo3::create_exception!(_native, FilePublicationError, PyOSError);

pub(crate) fn error(error: files::Error) -> PyErr {
    match error {
        files::Error::Invalid(message) => PyValueError::new_err(message),
        files::Error::Io(error) => error.into(),
        error @ files::Error::Published(_) => FilePublicationError::new_err(error.to_string()),
    }
}

#[pyclass(frozen, module = "asterion_bindings._native")]
pub(crate) struct ReadFilesHandle {
    pub(crate) inner: Arc<ReadRoot>,
}
#[pymethods]
impl ReadFilesHandle {
    #[new]
    fn new(py: Python<'_>, root: PathBuf, read_bytes: u64, scan_entries: usize) -> PyResult<Self> {
        let inner = py
            .detach(|| ReadRoot::new(&root, read_bytes, scan_entries))
            .map_err(error)?;
        Ok(Self {
            inner: Arc::new(inner),
        })
    }
    fn read<'py>(&self, py: Python<'py>, name: &str) -> PyResult<Bound<'py, PyBytes>> {
        let content = py.detach(|| self.inner.read(name)).map_err(error)?;
        Ok(PyBytes::new(py, &content))
    }
    fn digest(&self, py: Python<'_>, name: &str) -> PyResult<String> {
        py.detach(|| self.inner.digest(name)).map_err(error)
    }
    fn scan(&self, py: Python<'_>, name: &str, suffix: &str) -> PyResult<Vec<String>> {
        py.detach(|| self.inner.scan(name, suffix)).map_err(error)
    }
}

/// Write-once artifact grant; returns references as (name, sha256, bytes).
#[pyclass(frozen, module = "asterion_bindings._native")]
pub(crate) struct ArtifactStoreHandle {
    pub(crate) inner: ArtifactStore,
}
fn reference(value: ArtifactRef) -> (String, String, u64) {
    (value.name, value.sha256, value.bytes)
}
#[pymethods]
impl ArtifactStoreHandle {
    #[new]
    fn new(py: Python<'_>, root: PathBuf, max_bytes: u64, writable: bool) -> PyResult<Self> {
        let inner = py
            .detach(|| {
                if writable {
                    ArtifactStore::new(&root, max_bytes)
                } else {
                    ArtifactStore::reader(&root, max_bytes)
                }
            })
            .map_err(error)?;
        Ok(Self { inner })
    }
    #[pyo3(signature = (name, sha256, size=None))]
    fn verify(&self, py: Python<'_>, name: &str, sha256: &str, size: Option<u64>) -> PyResult<()> {
        py.detach(|| self.inner.verify(name, sha256, size))
            .map_err(error)
    }
    fn put(&self, py: Python<'_>, name: &str, content: &[u8]) -> PyResult<(String, String, u64)> {
        py.detach(|| self.inner.put(name, content))
            .map(reference)
            .map_err(error)
    }
    fn put_addressed(
        &self,
        py: Python<'_>,
        directory: &str,
        suffix: &str,
        content: &[u8],
    ) -> PyResult<(String, String, u64)> {
        py.detach(|| self.inner.put_addressed(directory, suffix, content))
            .map(reference)
            .map_err(error)
    }
    #[pyo3(signature = (name, sha256, size=None))]
    fn read<'py>(
        &self,
        py: Python<'py>,
        name: &str,
        sha256: &str,
        size: Option<u64>,
    ) -> PyResult<Bound<'py, PyBytes>> {
        let content = py
            .detach(|| self.inner.read(name, sha256, size))
            .map_err(error)?;
        Ok(PyBytes::new(py, &content))
    }
}

#[pyfunction]
fn file_digest(py: Python<'_>, path: PathBuf) -> PyResult<String> {
    py.detach(|| files::file_digest(&path)).map_err(error)
}

#[pyfunction]
fn trusted_tree_digest(
    py: Python<'_>,
    roots: Vec<PathBuf>,
    excluded_components: Vec<String>,
    excluded_suffixes: Vec<String>,
    max_entries: usize,
    max_bytes: u64,
) -> PyResult<String> {
    py.detach(|| {
        files::trusted_tree_digest(
            &roots,
            &excluded_components,
            &excluded_suffixes,
            max_entries,
            max_bytes,
        )
    })
    .map_err(error)
}

#[pyfunction]
fn file_atomic_write(py: Python<'_>, path: PathBuf, content: &[u8], replace: bool) -> PyResult<()> {
    py.detach(|| files::atomic_write(&path, content, replace))
        .map_err(error)
}

#[pyclass(frozen, module = "asterion_bindings._native")]
struct FileLockHandle {
    inner: Mutex<Option<FileLock>>,
}
#[pymethods]
impl FileLockHandle {
    #[new]
    #[pyo3(signature = (path, blocking, timeout=None))]
    fn new(py: Python<'_>, path: PathBuf, blocking: bool, timeout: Option<f64>) -> PyResult<Self> {
        let timeout = timeout.map(|seconds| {
            if !blocking || !seconds.is_finite() || !(0.0..=86400.0).contains(&seconds) {
                return Err(PyValueError::new_err("Timeout requires blocking acquisition and must be between zero and one day"));
            }
            Ok(Duration::from_secs_f64(seconds))
        }).transpose()?;
        let lock = py
            .detach(|| match timeout {
                Some(timeout) => FileLock::acquire_for(&path, timeout),
                None => FileLock::acquire(&path, blocking),
            })
            .map_err(error)?;
        Ok(Self {
            inner: Mutex::new(Some(lock)),
        })
    }
    fn close(&self, py: Python<'_>) {
        py.detach(|| {
            // There are no callbacks or fallible operations while holding this lock.
            self.inner.lock().expect("file lock handle").take();
        });
    }
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    let published = module.py().get_type::<FilePublicationError>();
    published.setattr("published", true)?;
    module.add("FilePublicationError", published)?;
    module.add_class::<ReadFilesHandle>()?;
    module.add_class::<FileLockHandle>()?;
    module.add_class::<ArtifactStoreHandle>()?;
    module.add_function(wrap_pyfunction!(file_digest, module)?)?;
    module.add_function(wrap_pyfunction!(trusted_tree_digest, module)?)?;
    module.add_function(wrap_pyfunction!(file_atomic_write, module)?)?;
    Ok(())
}
