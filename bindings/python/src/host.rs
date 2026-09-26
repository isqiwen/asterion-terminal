//! Python callback adaptation. All permission and lifecycle decisions live in L1.
use asterion_kernel::plugins::{Capability, Grants, Host, Manifest, Resource, ScopedHandle};
use pyo3::{exceptions::PyValueError, prelude::*};

fn input<T: serde::de::DeserializeOwned>(value: &str) -> PyResult<T> {
    serde_json::from_str(value).map_err(|error| PyValueError::new_err(error.to_string()))
}

#[pyclass(name = "HostHandle", module = "asterion_bindings._native")]
pub(crate) struct PyHandle {
    pub(crate) handle: ScopedHandle,
}

#[pymethods]
impl PyHandle {
    fn check(&self) -> PyResult<()> {
        self.handle.check().map_err(PyValueError::new_err)
    }
}

#[pyclass(name = "PluginHost", module = "asterion_bindings._native")]
struct PyHost {
    host: Host,
}

#[pymethods]
impl PyHost {
    #[new]
    fn new(manifests: &str) -> PyResult<Self> {
        let plugins: Vec<Manifest> = input(manifests)?;
        Ok(Self {
            host: Host::new(plugins).map_err(PyValueError::new_err)?,
        })
    }
    #[getter]
    fn order(&self) -> Vec<String> {
        self.host.plan().order.clone()
    }
    #[getter]
    fn type_order(&self) -> Vec<String> {
        self.host.plan().type_order.clone()
    }
    #[getter]
    fn state(&self) -> String {
        format!("{:?}", self.host.state())
    }
    fn begin(&mut self, grants: &str) -> PyResult<()> {
        let grants: Grants = input(grants)?;
        self.host.begin(&grants).map_err(PyValueError::new_err)
    }
    fn next_activation(&mut self) -> PyResult<Option<String>> {
        self.host.next_activation().map_err(PyValueError::new_err)
    }
    fn activated(&mut self, id: &str, exports: &str) -> PyResult<()> {
        let exports: Vec<Capability> = input(exports)?;
        self.host
            .activated(id, &exports)
            .map_err(PyValueError::new_err)
    }
    fn returned(&mut self, id: &str) -> PyResult<()> {
        self.host.returned(id).map_err(PyValueError::new_err)
    }
    fn finish(&mut self) -> PyResult<()> {
        self.host.finish().map_err(PyValueError::new_err)
    }
    fn close(&mut self) -> Vec<String> {
        self.host.close()
    }
    fn resource(&self, owner: &str, value: &str) -> PyResult<PyHandle> {
        let resource: Resource = input(value)?;
        self.host
            .resource_handle(owner, &resource)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn capability(&self, owner: &str, value: &str) -> PyResult<PyHandle> {
        let capability: Capability = input(value)?;
        self.host
            .capability_handle(owner, &capability)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn export(&self, value: &str) -> PyResult<PyHandle> {
        let capability: Capability = input(value)?;
        self.host
            .export_handle(&capability)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn observation(&self, owner: &str, name: &str) -> PyResult<PyHandle> {
        self.host
            .observation_handle(owner, name)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn context(&self, owner: &str) -> PyResult<PyHandle> {
        self.host
            .context_handle(owner)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
    fn plugin(&self, owner: &str) -> PyResult<PyHandle> {
        self.host
            .plugin_handle(owner)
            .map(|handle| PyHandle { handle })
            .map_err(PyValueError::new_err)
    }
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyHost>()?;
    module.add_class::<PyHandle>()?;
    Ok(())
}
