//! Monotonic ownership of resources returned by plugin and task dispatch.
//! Only the fixed host and execution-scope mechanisms can mint these handles.
use crate::{plugins::ScopedHandle, tasks::ExecutionHandle};

#[derive(Clone)]
pub enum Lifetime {
    Plugin(ScopedHandle),
    Execution(ExecutionHandle),
}

impl Lifetime {
    pub fn check(&self) -> Result<(), String> {
        match self {
            Self::Plugin(handle) => handle.check(),
            Self::Execution(handle) => handle.check(),
        }
    }
}

impl From<ScopedHandle> for Lifetime {
    fn from(value: ScopedHandle) -> Self {
        Self::Plugin(value)
    }
}

impl From<ExecutionHandle> for Lifetime {
    fn from(value: ExecutionHandle) -> Self {
        Self::Execution(value)
    }
}
