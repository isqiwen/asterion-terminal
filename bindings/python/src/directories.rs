//! Ownership and cancellation adapters for private directory staging.
use asterion_kernel::{directories::StagedDirectory as Directory, files};
use pyo3::{exceptions::PyValueError, prelude::*};
use std::path::PathBuf;
use std::sync::{Mutex, MutexGuard};

pub(crate) use crate::files::error;
pub(crate) fn interruptible<T: Send>(
    py: Python<'_>,
    action: impl FnOnce(&mut dyn FnMut() -> Result<(), files::Error>) -> Result<T, files::Error> + Send,
) -> PyResult<T> {
    let mut interruption = None;
    let result = py.detach(|| {
        action(&mut || {
            Python::attach(|py| py.check_signals()).map_err(|error| {
                interruption = Some(error);
                std::io::Error::from(std::io::ErrorKind::Interrupted).into()
            })
        })
    });
    match result {
        Err(failure) if failure.published() => Err(error(failure)),
        result => match interruption {
            Some(error) => Err(error),
            None => result.map_err(error),
        },
    }
}
#[pyclass(frozen, module = "asterion_bindings._native")]
pub(crate) struct StagedDirectory {
    inner: Mutex<Option<Directory>>,
}
impl StagedDirectory {
    pub(crate) fn lock(&self) -> PyResult<MutexGuard<'_, Option<Directory>>> {
        self.inner
            .try_lock()
            .map_err(|_| PyValueError::new_err("Staging is busy"))
    }
}
#[pymethods]
impl StagedDirectory {
    #[new]
    fn new(py: Python<'_>, destination: PathBuf, max_entries: usize) -> PyResult<Self> {
        Ok(Self {
            inner: Mutex::new(Some(
                py.detach(|| Directory::new(&destination, max_entries))
                    .map_err(error)?,
            )),
        })
    }
    #[getter]
    fn path(&self) -> PyResult<PathBuf> {
        Ok(self
            .lock()?
            .as_ref()
            .ok_or_else(|| PyValueError::new_err("Staging is closed"))?
            .path()
            .to_owned())
    }
    fn commit(&self, py: Python<'_>) -> PyResult<()> {
        let mut guard = self.lock()?;
        let stage = guard
            .as_mut()
            .ok_or_else(|| PyValueError::new_err("Staging is closed"))?;
        interruptible(py, |check| stage.commit_checked(check))
    }
    fn preserve(&self, py: Python<'_>) -> PyResult<PathBuf> {
        let stage = self
            .lock()?
            .take()
            .ok_or_else(|| PyValueError::new_err("Staging is closed"))?;
        py.detach(|| stage.preserve()).map_err(error)
    }
    fn close(&self, py: Python<'_>) -> PyResult<()> {
        let stage = self.lock()?.take();
        py.detach(|| drop(stage));
        Ok(())
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<StagedDirectory>()
}
