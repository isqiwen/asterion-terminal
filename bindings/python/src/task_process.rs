//! Values and Python callback adaptation for Rust-owned task computation.
use asterion_kernel::{
    communication,
    tasks::process::{self, Artifact, ComputationResult, Failure, TaskProcess},
};
use pyo3::{exceptions::PyValueError, prelude::*, types::PyBytes};
use std::{
    io,
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

#[pyclass(name = "TaskArtifact", module = "asterion_bindings._native")]
struct PyArtifact {
    artifact: Mutex<Artifact>,
}
#[pymethods]
impl PyArtifact {
    fn metadata(&self) -> PyResult<String> {
        let artifact = self
            .artifact
            .try_lock()
            .map_err(|_| error(Failure::Mechanism("busy")))?;
        serde_json::to_string(artifact.metadata())
            .map_err(|_| error(Failure::Mechanism("protocol")))
    }
    fn read_chunk<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyBytes>> {
        let content = py
            .detach(|| {
                self.artifact
                    .try_lock()
                    .map_err(|_| Failure::Mechanism("busy"))?
                    .read_chunk()
            })
            .map_err(error)?;
        Ok(PyBytes::new(py, &content))
    }
    fn close(&self, py: Python<'_>) {
        py.detach(|| {
            if let Ok(mut artifact) = self.artifact.lock() {
                artifact.close();
            }
        });
    }
}

#[pyclass(name = "TaskProcess", module = "asterion_bindings._native")]
struct PyProcess {
    process: Mutex<TaskProcess>,
    stopped: AtomicBool,
}
#[pymethods]
impl PyProcess {
    #[new]
    fn new(
        py: Python<'_>,
        arguments: Vec<String>,
        context: &str,
        request: &[u8],
        limits: &str,
    ) -> PyResult<Self> {
        let (program, arguments) = arguments
            .split_first()
            .ok_or_else(|| error(Failure::Mechanism("startup")))?;
        let mut command = Command::new(program);
        command.args(arguments);
        let context = communication::parse_context(
            serde_json::from_str(context).map_err(|_| error(Failure::Mechanism("protocol")))?,
        )
        .map_err(|_| error(Failure::Mechanism("protocol")))?;
        let limits =
            serde_json::from_str(limits).map_err(|_| error(Failure::Mechanism("protocol")))?;
        let process = py
            .detach(|| TaskProcess::spawn(command, context, request, limits))
            .map_err(error)?;
        Ok(Self {
            process: Mutex::new(process),
            stopped: AtomicBool::new(false),
        })
    }
    fn poll(&self, py: Python<'_>, timeout: f64) -> PyResult<bool> {
        let timeout = Duration::try_from_secs_f64(timeout)
            .map_err(|_| error(Failure::Mechanism("protocol")))?;
        let mut interrupted = None;
        let result = py.detach(|| {
            let mut process = self
                .process
                .try_lock()
                .map_err(|_| Failure::Mechanism("busy"))?;
            process.poll(timeout, &self.stopped, &mut || {
                if let Err(error) = Python::attach(|py| py.check_signals()) {
                    interrupted = Some(error);
                    true
                } else {
                    false
                }
            })
        });
        if let Some(error) = interrupted {
            return Err(error);
        }
        result.map_err(error)
    }
    fn take_result(&self, py: Python<'_>) -> PyResult<PyArtifact> {
        if self.stopped.load(Ordering::Acquire) {
            return Err(error(Failure::Mechanism("closed")));
        }
        let artifact = py
            .detach(|| {
                self.process
                    .try_lock()
                    .map_err(|_| Failure::Mechanism("busy"))?
                    .take_result()
            })
            .map_err(error)?;
        Ok(PyArtifact {
            artifact: Mutex::new(artifact),
        })
    }
    fn close(&self, py: Python<'_>) {
        self.stopped.store(true, Ordering::Release);
        py.detach(|| {
            if let Ok(mut process) = self.process.try_lock() {
                process.close();
            }
        });
    }
}
impl Drop for PyProcess {
    fn drop(&mut self) {
        if let Ok(process) = self.process.get_mut() {
            let _ = Python::try_attach(|py| py.detach(|| process.close()));
        }
    }
}

#[pyfunction]
fn task_process_serve(py: Python<'_>, callback: Py<PyAny>) -> PyResult<()> {
    py.detach(|| {
        process::serve(&mut io::stdin().lock(), &mut io::stdout().lock(), |input| {
            Python::attach(|py| {
                let callback_result = callback
                    .call1(py, (PyBytes::new(py, input),))
                    .and_then(|result| result.extract::<(Vec<u8>, String, Option<String>)>(py));
                match callback_result {
                    Ok((content, metadata, error)) => match serde_json::from_str(&metadata) {
                        Ok(metadata) => ComputationResult {
                            content,
                            metadata,
                            error,
                        },
                        Err(_) => ComputationResult {
                            content: vec![],
                            metadata: Default::default(),
                            error: Some("Invalid task result metadata".into()),
                        },
                    },
                    Err(_) => ComputationResult {
                        content: vec![],
                        metadata: Default::default(),
                        error: Some("Task execution failed".into()),
                    },
                }
            })
        })
    })
    .map_err(error)
}

pub fn register(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyProcess>()?;
    module.add_class::<PyArtifact>()?;
    module.add_function(wrap_pyfunction!(task_process_serve, module)?)?;
    Ok(())
}
