//! Opaque handles and byte conversion. Cryptographic operations stay in L1.
use asterion_kernel::secrets::Secrets;
use pyo3::{exceptions::PyValueError, prelude::*, types::PyBytes};

pyo3::create_exception!(_native, SecretError, PyValueError);

#[pyclass(frozen, module = "asterion_bindings._native")]
struct SecretHandle {
    inner: Secrets,
}

#[pymethods]
impl SecretHandle {
    #[new]
    fn new(master: &str, encryption: &[u8], fingerprint: &[u8]) -> Self {
        Self {
            inner: Secrets::new(master.as_bytes(), encryption, fingerprint),
        }
    }
    fn encrypt<'py>(&self, py: Python<'py>, content: &[u8]) -> Bound<'py, PyBytes> {
        let result = py.detach(|| self.inner.encrypt(content));
        PyBytes::new(py, &result)
    }
    fn decrypt<'py>(&self, py: Python<'py>, content: &[u8]) -> PyResult<Bound<'py, PyBytes>> {
        let result = py
            .detach(|| self.inner.decrypt(content))
            .map_err(|_| SecretError::new_err("Invalid encrypted content"))?;
        Ok(PyBytes::new(py, &result))
    }
    fn fingerprint(&self, py: Python<'_>, content: &[u8]) -> String {
        py.detach(|| self.inner.fingerprint(content))
    }
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<SecretHandle>()?;
    module.add("SecretError", module.py().get_type::<SecretError>())?;
    Ok(())
}
