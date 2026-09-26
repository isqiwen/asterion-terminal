//! Python conversion only. The journal, clocks and bounds belong to the kernel.
use asterion_kernel::diagnostics::{self, Component, EventCode, Observation};
use pyo3::{exceptions::PyValueError, prelude::*};
use std::path::PathBuf;
use std::sync::Mutex;

#[pyclass(name = "DiagnosticsObservation", module = "asterion_bindings._native")]
struct PyObservation {
    observation: Mutex<Observation>,
}
#[pymethods]
impl PyObservation {
    #[new]
    fn new(root: PathBuf, digest: &str) -> Self {
        Self {
            observation: Mutex::new(Observation::new(&root, digest)),
        }
    }
    fn call(&self, py: Python<'_>, method: &str) {
        py.detach(|| {
            if let Ok(mut observation) = self.observation.lock() {
                observation.call(method);
            }
        });
    }
    #[pyo3(signature = (code="success"))]
    fn finish(&self, py: Python<'_>, code: &str) {
        py.detach(|| {
            if let Ok(mut observation) = self.observation.lock() {
                observation.finish(code);
            }
        });
    }
}
#[pyfunction]
fn diagnostics_recent(py: Python<'_>, root: PathBuf, digest: &str) -> PyResult<String> {
    py.detach(|| {
        let rows = diagnostics::recent(&root, digest).map_err(PyValueError::new_err)?;
        serde_json::to_string(&rows)
            .map_err(|_| PyValueError::new_err("Execution diagnostics are unavailable"))
    })
}
#[pyfunction]
#[pyo3(signature = (root, component, code, context=None))]
fn diagnostic_event(
    py: Python<'_>,
    root: PathBuf,
    component: &str,
    code: &str,
    context: Option<&Bound<'_, PyAny>>,
) {
    let (Some(component), Some(code)) = (Component::parse(component), EventCode::parse(code))
    else {
        return;
    };
    let context = match context.filter(|value| !value.is_none()) {
        Some(value) => {
            let Ok(value) = crate::communication::value_from_python(value, &mut Vec::new()) else {
                return;
            };
            let Ok(trace) = asterion_kernel::communication::parse_context(value) else {
                return;
            };
            Some(trace)
        }
        None => None,
    };
    py.detach(|| diagnostics::record_event(&root, component, code, context.as_ref()));
}
#[pyfunction]
fn diagnostics_events(py: Python<'_>, root: PathBuf) -> PyResult<String> {
    py.detach(|| {
        let rows = diagnostics::events(&root).map_err(PyValueError::new_err)?;
        serde_json::to_string(&rows)
            .map_err(|_| PyValueError::new_err("Runtime diagnostics are unavailable"))
    })
}
pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyObservation>()?;
    module.add_function(wrap_pyfunction!(diagnostics_recent, module)?)?;
    module.add_function(wrap_pyfunction!(diagnostic_event, module)?)?;
    module.add_function(wrap_pyfunction!(diagnostics_events, module)?)?;
    Ok(())
}
