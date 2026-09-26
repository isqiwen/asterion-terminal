//! Python callback/value ownership for the fixed Rust transport.
use asterion_kernel::transport::{self, Failure, ProcessSession};
use pyo3::{exceptions::PyValueError, prelude::*};
use std::{
    io,
    path::PathBuf,
    process::Command,
    sync::{
        Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

fn error(failure: Failure) -> PyErr {
    PyValueError::new_err((failure.code(), failure.to_string()))
}
fn command(arguments: Vec<String>, cwd: Option<PathBuf>) -> PyResult<Command> {
    let (program, arguments) = arguments
        .split_first()
        .ok_or_else(|| error(Failure::startup()))?;
    let mut command = Command::new(program);
    command.args(arguments);
    if let Some(cwd) = cwd {
        command.current_dir(cwd);
    }
    Ok(command)
}
fn allowed(callback: Option<&Py<PyAny>>) -> bool {
    callback.is_none_or(|callback| {
        Python::attach(|py| {
            callback
                .call0(py)
                .and_then(|value| value.extract::<bool>(py))
                .unwrap_or(false)
        })
    })
}

#[pyfunction]
#[pyo3(signature = (arguments, request, cwd=None, authorized=None))]
fn transport_invoke(
    py: Python<'_>,
    arguments: Vec<String>,
    request: &str,
    cwd: Option<PathBuf>,
    authorized: Option<Py<PyAny>>,
) -> PyResult<String> {
    let command = command(arguments, cwd)?;
    let request = transport::decode_request(request.as_bytes()).map_err(error)?;
    py.detach(|| transport::invoke(command, &request, &mut || allowed(authorized.as_ref())))
        .map(|value| value.to_string())
        .map_err(error)
}

#[pyclass(name = "ProcessTransport", module = "asterion_bindings._native")]
struct PyProcess {
    process: Mutex<ProcessSession>,
    authorized: Py<PyAny>,
    closed: AtomicBool,
}
#[pymethods]
impl PyProcess {
    #[new]
    fn new(
        py: Python<'_>,
        arguments: Vec<String>,
        cwd: PathBuf,
        timeout: f64,
        authorized: Py<PyAny>,
    ) -> PyResult<Self> {
        let command = command(arguments, Some(cwd))?;
        let lifetime =
            Duration::try_from_secs_f64(timeout).map_err(|_| error(Failure::protocol()))?;
        let permitted = allowed(Some(&authorized));
        let process = py
            .detach(|| ProcessSession::spawn(command, lifetime, permitted))
            .map_err(error)?;
        Ok(Self {
            process: Mutex::new(process),
            authorized,
            closed: AtomicBool::new(false),
        })
    }
    fn call(&self, py: Python<'_>, request: &str) -> PyResult<String> {
        let request = match transport::decode_request(request.as_bytes()) {
            Ok(request) => request,
            Err(failure) => {
                self.close(py);
                return Err(error(failure));
            }
        };
        py.detach(|| {
            let mut process = self.process.try_lock().map_err(|_| Failure::protocol())?;
            process.call(&request, &mut || {
                !self.closed.load(Ordering::Acquire)
                    && allowed(Some(&self.authorized))
                    && !self.closed.load(Ordering::Acquire)
            })
        })
        .map(|value| value.to_string())
        .map_err(error)
    }
    fn close(&self, py: Python<'_>) {
        self.closed.store(true, Ordering::Release);
        py.detach(|| {
            // A callback may close its own active call. Never wait on that call's
            // mutex: the closed flag makes its next boundary check reap the child.
            if let Ok(mut process) = self.process.try_lock() {
                process.close();
            }
        });
    }
}

#[pyfunction]
fn transport_child_limits() -> PyResult<()> {
    transport::child_limits().map_err(error)
}

#[pyfunction]
fn transport_serve(py: Python<'_>, dispatch: Py<PyAny>) -> PyResult<()> {
    py.detach(|| {
        let stdin = io::stdin();
        let stdout = io::stdout();
        transport::serve(&mut stdin.lock(), &mut stdout.lock(), |request| {
            Python::attach(|py| {
                let raw = dispatch
                    .call1(py, (request.to_string(),))
                    .and_then(|value| value.extract::<String>(py))
                    .map_err(|_| ())?;
                transport::decode_result(raw.as_bytes()).map_err(|_| ())
            })
        })
    })
    .map_err(error)
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_function(wrap_pyfunction!(transport_invoke, module)?)?;
    module.add_function(wrap_pyfunction!(transport_child_limits, module)?)?;
    module.add_function(wrap_pyfunction!(transport_serve, module)?)?;
    module.add_class::<PyProcess>()?;
    Ok(())
}
