//! One-use restore operations and backup evidence accounting, without domain policy.
use serde_json::Value;
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::{Arc, Mutex},
    thread::{self, ThreadId},
};

type Result<T> = std::result::Result<T, &'static str>;
const CLOSED: &str = "Restore scope is closed";
const LIMIT: usize = 4096;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Active,
    Closed,
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum Status {
    Pending,
    Running,
    Complete,
    Failed,
}
struct State {
    phase: Phase,
    owner: ThreadId,
    operations: Vec<Status>,
}
impl State {
    fn check(&self) -> Result<()> {
        if self.phase != Phase::Active {
            return Err(CLOSED);
        }
        if self.owner != thread::current().id() {
            return Err("Restore scope belongs to another thread");
        }
        Ok(())
    }
}

/// Only this owner can register operations; dropping it revokes retained handles.
pub struct Scope {
    state: Arc<Mutex<State>>,
}
impl Default for Scope {
    fn default() -> Self {
        Self::new()
    }
}
impl Scope {
    pub fn new() -> Self {
        Self {
            state: Arc::new(Mutex::new(State {
                phase: Phase::Active,
                owner: thread::current().id(),
                operations: Vec::new(),
            })),
        }
    }
    pub fn operation(&self) -> Result<Operation> {
        let mut state = self.state.lock().map_err(|_| CLOSED)?;
        state.check()?;
        if state.operations.len() == LIMIT {
            return Err("Restore operation budget exceeded");
        }
        let index = state.operations.len();
        state.operations.push(Status::Pending);
        Ok(Operation {
            state: self.state.clone(),
            index,
        })
    }
    /// Success seals the scope before the surrounding driver transaction commits.
    pub fn verify(&self) -> Result<()> {
        let mut state = self.state.lock().map_err(|_| CLOSED)?;
        state.check()?;
        if state
            .operations
            .iter()
            .any(|status| *status != Status::Complete)
        {
            return Err("Required restore operations did not complete");
        }
        state.phase = Phase::Closed;
        Ok(())
    }
    pub fn close(&self) {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .phase = Phase::Closed;
    }
}
impl Drop for Scope {
    fn drop(&mut self) {
        self.close();
    }
}

/// Private state and index prevent callers from fabricating completion records.
pub struct Operation {
    state: Arc<Mutex<State>>,
    index: usize,
}
#[derive(Debug, PartialEq, Eq)]
pub enum InvokeError<E> {
    Mechanism(&'static str),
    Callback(E),
}
impl Operation {
    pub fn invoke<E>(
        &self,
        action: impl FnOnce() -> std::result::Result<(), E>,
    ) -> std::result::Result<(), InvokeError<E>> {
        {
            let mut state = self
                .state
                .lock()
                .map_err(|_| InvokeError::Mechanism(CLOSED))?;
            state.check().map_err(InvokeError::Mechanism)?;
            if state.operations[self.index] != Status::Pending {
                state.operations[self.index] = Status::Failed;
                return Err(InvokeError::Mechanism(
                    "Restore operation was already invoked",
                ));
            }
            state.operations[self.index] = Status::Running;
        }
        // Never hold the lock across plugin code. Recursive calls observe Running
        // immediately; a retained handle cannot run on a different thread.
        let result = action();
        let mut state = self
            .state
            .lock()
            .map_err(|_| InvokeError::Mechanism(CLOSED))?;
        if let Err(error) = result {
            state.operations[self.index] = Status::Failed;
            return Err(InvokeError::Callback(error));
        }
        if state.operations[self.index] != Status::Running {
            return Err(InvokeError::Mechanism(
                "Required restore operations did not complete",
            ));
        }
        state.operations[self.index] = Status::Complete;
        state.check().map_err(InvokeError::Mechanism)
    }
}

pub fn inputs(expected: &[String], provided: &[String], restore: bool) -> Result<()> {
    let expected_set: BTreeSet<_> = expected.iter().collect();
    let provided_set: BTreeSet<_> = provided.iter().collect();
    if expected_set.len() != expected.len() || provided_set.len() != provided.len() {
        return Err("Duplicate recovery declaration");
    }
    if expected_set != provided_set {
        return Err(if restore {
            "Restore inputs do not match steps"
        } else {
            "Backup inputs do not match validators"
        });
    }
    Ok(())
}

#[derive(Default)]
pub struct Metrics {
    counts: BTreeMap<String, u64>,
}
impl Metrics {
    pub fn add(&mut self, value: Value) -> Result<()> {
        let values = value
            .as_object()
            .ok_or("Invalid backup validation metrics")?;
        // Validate the whole contribution before adding anything.
        for (name, value) in values {
            if value.as_u64().is_none() {
                return Err("Invalid backup validation metrics");
            }
            if self.counts.contains_key(name) {
                return Err("Duplicate backup validation metric");
            }
        }
        self.counts.extend(
            values
                .iter()
                .map(|(name, value)| (name.clone(), value.as_u64().unwrap())),
        );
        Ok(())
    }
    pub fn counts(&self) -> &BTreeMap<String, u64> {
        &self.counts
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn required_operations_are_one_use_and_success_seals_the_scope() {
        let scope = Scope::new();
        let operation = scope.operation().unwrap();
        assert!(scope.verify().is_err());
        operation.invoke(|| Ok::<_, ()>(())).unwrap();
        scope.verify().unwrap();
        assert!(scope.operation().is_err());
        assert!(scope.verify().is_err());
    }
    #[test]
    fn swallowed_callback_error_and_panics_cannot_be_verified() {
        let scope = Scope::new();
        let operation = scope.operation().unwrap();
        assert_eq!(
            operation.invoke(|| Err("failed")),
            Err(InvokeError::Callback("failed"))
        );
        assert!(scope.verify().is_err());
        let scope = Scope::new();
        let operation = scope.operation().unwrap();
        assert!(
            std::panic::catch_unwind(
                || operation.invoke(|| -> std::result::Result<(), ()> { panic!("callback panic") })
            )
            .is_err()
        );
        assert!(scope.verify().is_err());
    }
    #[test]
    fn dropping_owner_or_closing_inside_callback_revokes_operations() {
        let operation = {
            let scope = Scope::new();
            scope.operation().unwrap()
        };
        assert_eq!(
            operation.invoke(|| Ok::<_, ()>(())),
            Err(InvokeError::Mechanism(CLOSED))
        );
        let scope = Scope::new();
        let operation = scope.operation().unwrap();
        assert_eq!(
            operation.invoke(|| {
                scope.close();
                Ok::<_, ()>(())
            }),
            Err(InvokeError::Mechanism(CLOSED))
        );
    }
    #[test]
    fn threads_and_reentrant_calls_cannot_execute_the_same_action() {
        let scope = Scope::new();
        let operation = Arc::new(scope.operation().unwrap());
        let foreign = operation.clone();
        assert!(
            thread::spawn(move || foreign.invoke(|| Ok::<_, ()>(())).is_err())
                .join()
                .unwrap()
        );
        assert!(
            operation
                .invoke(|| {
                    assert!(operation.invoke(|| Ok::<_, ()>(())).is_err());
                    Ok::<_, ()>(())
                })
                .is_err()
        );
        assert!(scope.verify().is_err());
    }
    #[test]
    fn caught_duplicate_operation_still_prevents_commit() {
        let scope = Scope::new();
        let operation = scope.operation().unwrap();
        operation.invoke(|| Ok::<_, ()>(())).unwrap();
        assert!(operation.invoke(|| Ok::<_, ()>(())).is_err());
        assert!(scope.verify().is_err());
    }
    #[test]
    fn declarations_and_metrics_cannot_be_omitted_duplicated_or_partly_merged() {
        assert!(inputs(&["one".into()], &[], false).is_err());
        assert!(inputs(&["one".into(), "one".into()], &["one".into()], true).is_err());
        assert!(inputs(&["one".into()], &["one".into()], true).is_ok());
        let mut metrics = Metrics::default();
        metrics.add(json!({"records": 10})).unwrap();
        for invalid in [
            json!({"extra": 1, "records": 2}),
            json!({"bad": true}),
            json!({"bad": -1}),
            json!({"bad": 1.0}),
            Value::Null,
        ] {
            assert!(metrics.add(invalid).is_err());
        }
        assert_eq!(metrics.counts(), &BTreeMap::from([("records".into(), 10)]));
    }
}
