//! Python byte/value conversion for bounded archives and immutable objects.
use asterion_kernel::{
    archive::{self, CheckedArchive, Policy},
    artifacts,
};
use pyo3::{exceptions::PyValueError, prelude::*, types::PyBytes};
use std::path::PathBuf;
fn error(error: archive::Error) -> PyErr {
    match error {
        archive::Error::Invalid(message) => PyValueError::new_err(message),
        archive::Error::Files(error) => crate::files::error(error),
    }
}
fn policy(value: &str) -> PyResult<Policy> {
    serde_json::from_str(value).map_err(|_| PyValueError::new_err("Invalid archive policy"))
}
#[pyclass(frozen, module = "asterion_bindings._native")]
struct ArchiveHandle {
    inner: CheckedArchive,
}
#[pymethods]
impl ArchiveHandle {
    #[new]
    fn new(py: Python<'_>, content: &[u8], policy_json: &str) -> PyResult<Self> {
        let policy = policy(policy_json)?;
        Ok(Self {
            inner: py
                .detach(|| CheckedArchive::check(content, &policy))
                .map_err(error)?,
        })
    }
    #[getter]
    fn digest(&self) -> &str {
        self.inner.digest()
    }
    #[getter]
    fn names(&self) -> Vec<String> {
        self.inner.names()
    }
    fn read<'py>(&self, py: Python<'py>, name: &str) -> PyResult<Bound<'py, PyBytes>> {
        Ok(PyBytes::new(py, self.inner.read(name).map_err(error)?))
    }
    fn publish(&self, py: Python<'_>, root: PathBuf) -> PyResult<()> {
        py.detach(|| artifacts::publish(&root, &self.inner))
            .map_err(error)
    }
    #[staticmethod]
    fn load(py: Python<'_>, root: PathBuf, digest: &str, policy_json: &str) -> PyResult<Self> {
        let policy = policy(policy_json)?;
        Ok(Self {
            inner: py
                .detach(|| artifacts::load(&root, digest, &policy))
                .map_err(error)?,
        })
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<ArchiveHandle>()
}
