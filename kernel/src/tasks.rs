//! Task state and lease admission. SQL/transaction execution is a separate
//! driver: it must apply the returned compare-and-set predicate and publish the
//! requested event in the same transaction as domain changes.

pub mod handlers;
pub mod process;
pub mod repository;

use crate::plugins::Resource;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};
use std::{
    collections::BTreeSet,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

macro_rules! states {
    ($($state:ident),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
        #[serde(rename_all = "SCREAMING_SNAKE_CASE")]
        pub enum State { $($state),+ }
        impl State { pub const ALL: &'static [State] = &[$(State::$state),+]; }
    };
}
states!(Queued, Running, Succeeded, Failed, Cancelled);

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Snapshot {
    pub id: String,
    pub state: State,
    pub attempt: i64,
    pub token: Option<String>,
    pub lease_until: Option<f64>,
    pub result: Option<Map<String, Value>>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Column {
    Id,
    State,
    Attempt,
    Token,
    LeaseUntil,
    Kind,
}

/// Restricted query representation for this task table, never arbitrary SQL.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Predicate {
    Eq { column: Column, value: Value },
    Le { column: Column, value: f64 },
    In { column: Column, values: Vec<String> },
    Not { clause: Box<Predicate> },
    And { clauses: Vec<Predicate> },
    Or { clauses: Vec<Predicate> },
}

#[derive(Debug, Serialize)]
pub struct Plan {
    pub condition: Predicate,
    pub values: Map<String, Value>,
    pub event: bool,
}

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Action {
    Claim {
        worker_id: String,
        token: String,
        lease_seconds: f64,
    },
    Heartbeat {
        token: String,
        lease_seconds: f64,
    },
    Complete {
        token: String,
        result: Map<String, Value>,
    },
    Progress {
        token: String,
        result: Map<String, Value>,
    },
    Fail {
        token: String,
        error: String,
    },
    Cancel,
    Isolate {
        reason: String,
    },
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TransitionRequest {
    pub row: Snapshot,
    pub action: Action,
    pub now: f64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordRequest {
    pub id: String,
    pub command_id: String,
    pub kind: String,
    pub payload: Map<String, Value>,
    pub now: f64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReuseRequest {
    pub original_kind: String,
    pub original_payload: Map<String, Value>,
    pub kind: String,
    pub payload: Map<String, Value>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LeaseDurationRequest {
    pub lease_seconds: f64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EmptyRequest {}

pub fn check_duration(seconds: f64) -> Result<(), String> {
    if !seconds.is_finite() || seconds <= 0.0 {
        return Err("Task lease duration must be positive and finite".into());
    }
    Ok(())
}

fn check_time(now: f64) -> Result<(), String> {
    if !now.is_finite() || now < 0.0 {
        return Err("Invalid task clock".into());
    }
    Ok(())
}

fn lease_end(now: f64, seconds: f64) -> Result<f64, String> {
    check_duration(seconds)?;
    let end = now + seconds;
    if !end.is_finite() || end <= now {
        return Err("Task lease deadline is not representable".into());
    }
    Ok(end)
}

pub fn record(request: RecordRequest) -> Result<Value, String> {
    check_time(request.now)?;
    if request.id.is_empty() || request.command_id.is_empty() || request.kind.is_empty() {
        return Err("Task identity, command and kind must be nonempty".into());
    }
    Ok(
        json!({"id":request.id,"command_id":request.command_id,"kind":request.kind,
        "payload":request.payload,"state":State::Queued,"attempt":0,"created_at":request.now}),
    )
}

pub fn check_reuse(request: &ReuseRequest) -> Result<(), String> {
    if request.original_kind != request.kind || request.original_payload != request.payload {
        return Err("command_id reused with different input".into());
    }
    Ok(())
}

pub fn require_lease(row: Option<&Snapshot>, token: &str, now: f64) -> Result<(), String> {
    check_time(now)?;
    if row.is_none_or(|row| {
        row.id.is_empty()
            || row.attempt <= 0
            || row.state != State::Running
            || token.is_empty()
            || row.token.as_deref() != Some(token)
            || row
                .lease_until
                .is_none_or(|until| !until.is_finite() || until <= now)
    }) {
        return Err("Lease expired, cancelled, or superseded".into());
    }
    Ok(())
}

fn eq(column: Column, value: impl Serialize) -> Predicate {
    Predicate::Eq {
        column,
        value: serde_json::to_value(value).expect("task scalar"),
    }
}

/// Which task kinds one executor claims. Each kind is executed by exactly one
/// executor; the application assigns disjoint scopes.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(
    tag = "scope",
    content = "kinds",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum ClaimKinds {
    Only(BTreeSet<String>),
    Except(BTreeSet<String>),
}

pub fn claim_filter(now: f64, kinds: Option<&ClaimKinds>) -> Result<Predicate, String> {
    check_time(now)?;
    let due = claimable(now);
    let scope = match kinds {
        None => return Ok(due),
        Some(ClaimKinds::Only(kinds)) if kinds.is_empty() => {
            return Err("A claim scope needs at least one task kind".into());
        }
        Some(ClaimKinds::Only(kinds)) => Predicate::In {
            column: Column::Kind,
            values: kinds.iter().cloned().collect(),
        },
        Some(ClaimKinds::Except(kinds)) if kinds.is_empty() => return Ok(due),
        Some(ClaimKinds::Except(kinds)) => Predicate::Not {
            clause: Box::new(Predicate::In {
                column: Column::Kind,
                values: kinds.iter().cloned().collect(),
            }),
        },
    };
    Ok(Predicate::And {
        clauses: vec![due, scope],
    })
}

fn claimable(now: f64) -> Predicate {
    Predicate::Or {
        clauses: vec![
            eq(Column::State, State::Queued),
            Predicate::And {
                clauses: vec![
                    eq(Column::State, State::Running),
                    Predicate::Le {
                        column: Column::LeaseUntil,
                        value: now,
                    },
                ],
            },
        ],
    }
}

fn fence(row: &Snapshot) -> Predicate {
    Predicate::And {
        clauses: vec![
            eq(Column::Id, &row.id),
            eq(Column::State, row.state),
            eq(Column::Attempt, row.attempt),
            eq(Column::Token, &row.token),
            eq(Column::LeaseUntil, row.lease_until),
        ],
    }
}

pub fn transition(request: TransitionRequest) -> Result<Option<Plan>, String> {
    check_time(request.now)?;
    let row = &request.row;
    if row.id.is_empty()
        || row.attempt < 0
        || row.lease_until.is_some_and(|until| !until.is_finite())
    {
        return Err("Invalid task snapshot".into());
    }
    let values;
    let mut event = true;
    match request.action {
        Action::Claim {
            worker_id,
            token,
            lease_seconds,
        } => {
            let eligible = row.state == State::Queued
                || (row.state == State::Running
                    && row.lease_until.is_some_and(|until| until <= request.now));
            if !eligible {
                return Ok(None);
            }
            if worker_id.is_empty() || token.is_empty() || row.token.as_deref() == Some(&token) {
                return Err("Claim requires a worker and a fresh lease token".into());
            }
            let attempt = row.attempt.checked_add(1).ok_or("Task attempt overflow")?;
            values = json!({"state":State::Running,"token":token,"attempt":attempt,
                "worker_id":worker_id,"lease_until":lease_end(request.now,lease_seconds)?,"error":null});
        }
        Action::Heartbeat {
            token,
            lease_seconds,
        } => {
            require_lease(Some(row), &token, request.now)?;
            // A backwards wall-clock adjustment must not shorten an existing lease.
            let until =
                lease_end(request.now, lease_seconds)?.max(row.lease_until.expect("checked lease"));
            values = json!({"lease_until":until});
            event = false;
        }
        Action::Complete { token, result } => {
            require_lease(Some(row), &token, request.now)?;
            values = json!({"state":State::Succeeded,"result":result});
        }
        Action::Progress { token, result } => {
            require_lease(Some(row), &token, request.now)?;
            if row.result.as_ref() == Some(&result) {
                return Ok(None);
            }
            values = json!({"result":result});
        }
        Action::Fail { token, error } => {
            require_lease(Some(row), &token, request.now)?;
            values =
                json!({"state":State::Failed,"error":error.chars().take(2000).collect::<String>()});
        }
        Action::Cancel => {
            if !matches!(row.state, State::Queued | State::Running) {
                return Ok(None);
            }
            values = json!({"state":State::Cancelled});
        }
        Action::Isolate { reason } => {
            if !matches!(row.state, State::Queued | State::Running) {
                return Ok(None);
            }
            values = json!({"state":State::Cancelled,"token":null,"worker_id":null,
                "lease_until":null,"error":reason.chars().take(2000).collect::<String>()});
        }
    }
    Ok(Some(Plan {
        condition: fence(row),
        values: values.as_object().expect("task update object").clone(),
        event,
    }))
}

/// Fixed per-execution grants: no plugin registration is involved.
pub struct ExecutionScope {
    declared: BTreeSet<Resource>,
    live: Arc<AtomicBool>,
}
#[derive(Clone)]
pub struct ExecutionHandle {
    live: Arc<AtomicBool>,
}

impl ExecutionHandle {
    pub fn check(&self) -> Result<(), String> {
        if self.live.load(Ordering::Acquire) {
            Ok(())
        } else {
            Err("Execution context is closed".into())
        }
    }
}

impl ExecutionScope {
    pub fn new(declarations: Vec<Resource>, grants: Vec<Resource>) -> Result<Self, String> {
        let ids: BTreeSet<_> = declarations.iter().map(|resource| &resource.id).collect();
        if ids.len() != declarations.len() {
            return Err("Duplicate execution resource".into());
        }
        if declarations
            .iter()
            .any(|resource| resource.id.is_empty() || resource.contract.is_empty())
        {
            return Err("Invalid execution resource declaration".into());
        }
        let declared: BTreeSet<_> = declarations.into_iter().collect();
        let granted: BTreeSet<_> = grants.iter().cloned().collect();
        if declared != granted || granted.len() != grants.len() {
            return Err("Execution resources do not match grants".into());
        }
        Ok(Self {
            declared,
            live: Arc::new(AtomicBool::new(true)),
        })
    }
    pub fn resource(&self, resource: &Resource) -> Result<ExecutionHandle, String> {
        let handle = ExecutionHandle {
            live: self.live.clone(),
        };
        handle.check()?;
        if !self.declared.contains(resource) {
            return Err(format!(
                "Execution resource is not granted: {}",
                resource.id
            ));
        }
        Ok(handle)
    }
    pub fn close(&mut self) {
        self.live.store(false, Ordering::Release);
    }
}
impl Drop for ExecutionScope {
    fn drop(&mut self) {
        self.close();
    }
}
