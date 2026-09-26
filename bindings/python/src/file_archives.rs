//! Thin Python ownership and value conversion for streaming archives.
use crate::directories::{StagedDirectory, interruptible};
use asterion_kernel::file_archives::{Evidence, Policy, Reader, Writer};
use pyo3::{exceptions::PyValueError, prelude::*, types::PyBytes};
use std::{collections::BTreeMap, path::PathBuf, sync::Mutex};
fn policy(value: &str) -> PyResult<Policy> {
    serde_json::from_str(value).map_err(|_| PyValueError::new_err("Invalid archive limits"))
}
fn closed() -> PyErr {
    PyValueError::new_err("Archive handle is closed or busy")
}
#[pyclass(frozen, module = "asterion_bindings._native")]
struct FileArchiveWriter {
    inner: Mutex<Option<Writer>>,
}
#[pymethods]
impl FileArchiveWriter {
    #[new]
    fn new(
        py: Python<'_>,
        source: PathBuf,
        destination: PathBuf,
        roots: Vec<String>,
        excludes: Vec<String>,
        policy_json: &str,
        metadata_name: &str,
    ) -> PyResult<Self> {
        let policy = policy(policy_json)?;
        let writer = interruptible(py, |check| {
            Writer::capture(
                &source,
                &destination,
                &roots,
                &excludes,
                policy,
                metadata_name,
                check,
            )
        })?;
        Ok(Self {
            inner: Mutex::new(Some(writer)),
        })
    }
    fn catalog(&self) -> PyResult<String> {
        let guard = self.inner.try_lock().map_err(|_| closed())?;
        let catalog = guard
            .as_ref()
            .ok_or_else(closed)?
            .catalog()
            .map_err(crate::directories::error)?;
        serde_json::to_string(catalog).map_err(|_| PyValueError::new_err("Invalid archive catalog"))
    }
    fn commit(&self, py: Python<'_>, metadata: &[u8]) -> PyResult<String> {
        let mut guard = self.inner.try_lock().map_err(|_| closed())?;
        let writer = guard.as_mut().ok_or_else(closed)?;
        let result = interruptible(py, |check| writer.commit(metadata, check))?;
        serde_json::to_string(&result).map_err(|_| PyValueError::new_err("Invalid archive result"))
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        let writer = self.inner.try_lock().map_err(|_| closed())?.take();
        py.detach(|| drop(writer));
        Ok(())
    }
}
#[pyclass(frozen, module = "asterion_bindings._native")]
struct FileArchiveReader {
    inner: Mutex<Option<Reader>>,
}
#[pymethods]
impl FileArchiveReader {
    #[new]
    fn new(
        py: Python<'_>,
        path: PathBuf,
        policy_json: &str,
        metadata_name: &str,
    ) -> PyResult<Self> {
        let policy = policy(policy_json)?;
        Ok(Self {
            inner: Mutex::new(Some(interruptible(py, |check| {
                Reader::open(&path, policy, metadata_name, check)
            })?)),
        })
    }
    fn metadata<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyBytes>> {
        let mut guard = self.inner.try_lock().map_err(|_| closed())?;
        let reader = guard.as_mut().ok_or_else(closed)?;
        let value = interruptible(py, |check| reader.metadata(check))?;
        Ok(PyBytes::new(py, &value))
    }
    fn digest(&self, py: Python<'_>) -> PyResult<String> {
        let mut guard = self.inner.try_lock().map_err(|_| closed())?;
        let reader = guard.as_mut().ok_or_else(closed)?;
        interruptible(py, |check| reader.digest(check))
    }
    fn extract(
        &self,
        py: Python<'_>,
        stage: &StagedDirectory,
        files_json: &str,
        directories_json: &str,
    ) -> PyResult<()> {
        let files: BTreeMap<String, Evidence> = serde_json::from_str(files_json)
            .map_err(|_| PyValueError::new_err("Invalid archive file evidence"))?;
        let directories: Vec<String> = serde_json::from_str(directories_json)
            .map_err(|_| PyValueError::new_err("Invalid archive directory evidence"))?;
        let mut guard = self.inner.try_lock().map_err(|_| closed())?;
        let reader = guard.as_mut().ok_or_else(closed)?;
        let stage_guard = stage.lock()?;
        let directory = stage_guard.as_ref().ok_or_else(closed)?;
        interruptible(py, |check| {
            reader.extract(directory, &files, &directories, check)
        })
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        let reader = self.inner.try_lock().map_err(|_| closed())?.take();
        py.detach(|| drop(reader));
        Ok(())
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<FileArchiveWriter>()?;
    module.add_class::<FileArchiveReader>()
}
