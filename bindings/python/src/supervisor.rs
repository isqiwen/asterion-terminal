use asterion_kernel::supervisor::{Configuration, Error, Supervisor};
use pyo3::{exceptions::PyValueError, prelude::*};

#[pyclass(name = "Supervisor", module = "asterion_bindings._native")]
struct PySupervisor {
    supervisor: Supervisor,
}
#[pymethods]
impl PySupervisor {
    #[new]
    fn new(configuration: &str) -> PyResult<Self> {
        let configuration: Configuration = serde_json::from_str(configuration)
            .map_err(|_| PyValueError::new_err("Invalid supervisor configuration"))?;
        Ok(Self {
            supervisor: Supervisor::new(configuration).map_err(PyValueError::new_err)?,
        })
    }
    fn request_stop(&self) {
        self.supervisor.request_stop();
    }
    fn run(&self, py: Python<'_>) -> PyResult<()> {
        py.detach(|| {
            self.supervisor
                .run(&mut || Python::attach(|py| py.check_signals()))
        })
        .map_err(|error| match error {
            Error::Mechanism(message) => PyValueError::new_err(message),
            Error::Callback(error) => error,
        })
    }
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PySupervisor>()
}
