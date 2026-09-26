//! Collection of a claimed sync task: resume verified evidence, fetch every
//! other partition from the source outside any transaction, retain each
//! observation under the task's lease, and produce the evidence the task
//! publishes. Only declared fields enter the evidence store.
use crate::configuration;
use crate::credentials::Credentials;
use crate::ids::canonical;
use crate::observations::{self, EvidenceError, Observation};
use crate::providers;
use crate::sync::{self, KIND};
use asterion_data_store::provider::{Provider, SyncRequest};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::tasks::Action;
use asterion_kernel::tasks::repository::{Repository, Request};
use asterion_store::{Store, Transaction};
use chrono::{SecondsFormat, Utc};
use serde_json::{Map, Value, json};
use std::time::{SystemTime, UNIX_EPOCH};

/// Largest evidence one task may publish, in canonical bytes.
pub const MAX_EVIDENCE: usize = 7_500_000;
const ERROR_CODES: [&str; 4] = [
    "AUTH_FAILED",
    "PERMISSION_DENIED",
    "RATE_LIMITED",
    "PROVIDER_REJECTED",
];

#[derive(Debug)]
pub enum CollectError {
    /// The lease is no longer held: stop without touching the task.
    Lease(String),
    /// The task fails with this user-facing reason.
    Failed(String),
}

type Result<T> = std::result::Result<T, CollectError>;

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

/// The instant of observation, as Python's `datetime.now(UTC).isoformat()`.
fn observed_at() -> String {
    Utc::now()
        .to_rfc3339_opts(SecondsFormat::Micros, false)
        .replace(".000000+", "+")
}

fn evidence_error(error: EvidenceError) -> CollectError {
    match error {
        EvidenceError::Refused(message) => CollectError::Failed(message),
        EvidenceError::Conflict(message) => CollectError::Lease(message),
        EvidenceError::NotFound => CollectError::Failed("采集任务不存在".into()),
        EvidenceError::Store(_) => CollectError::Lease("数据库不可用".into()),
    }
}

fn source_error(error: configuration::SourceError) -> CollectError {
    match error {
        configuration::SourceError::Refused(message) => CollectError::Failed(message),
        configuration::SourceError::Conflict(message) => CollectError::Lease(message),
        configuration::SourceError::Store(_) => CollectError::Lease("数据库不可用".into()),
    }
}

/// One short write transaction of a collection step.
fn step<T>(store: &Store, work: impl FnOnce(&Transaction) -> Result<T>) -> Result<T> {
    store.transaction(true, work)
}
impl From<asterion_store::StoreError> for CollectError {
    fn from(_: asterion_store::StoreError) -> Self {
        Self::Lease("数据库不可用".into())
    }
}

/// Everything a collection needs from its host.
pub struct Host<'a> {
    pub store: &'a Store,
    pub artifacts: &'a ArtifactStore,
    pub tasks: &'a Repository,
    pub credentials: &'a Credentials,
    pub lease_seconds: f64,
    pub trace: &'a Value,
}

impl Host<'_> {
    fn apply(&self, job_id: &str, action: Action) -> Result<()> {
        step(self.store, |tx| {
            self.tasks
                .run(
                    tx.connection(),
                    Request::Apply {
                        id: job_id.into(),
                        action,
                        now: now(),
                    },
                    self.trace.clone(),
                )
                .map(|_| ())
                .map_err(|error| CollectError::Lease(error.to_string()))
        })
    }

    fn progress(&self, job_id: &str, token: &str, completed: usize, total: usize) -> Result<()> {
        step(self.store, |tx| {
            sync::progress(
                tx,
                self.tasks,
                job_id,
                token,
                completed as i64,
                total as i64,
                now(),
                self.trace,
            )
            .map_err(source_error)
        })?;
        self.apply(
            job_id,
            Action::Heartbeat {
                token: token.into(),
                lease_seconds: self.lease_seconds,
            },
        )
    }

    fn record(&self, job_id: &str, token: &str, index: usize, value: &Value) -> Result<()> {
        let observation = Observation::parse(value).map_err(evidence_error)?;
        step(self.store, |tx| {
            observations::record(
                tx,
                self.artifacts,
                self.tasks,
                job_id,
                token,
                index as i64,
                &observation,
                now(),
                self.trace,
            )
            .map(|_| ())
            .map_err(evidence_error)
        })
    }

    /// Fail the task with `reason`, unless its lease was already lost.
    pub fn fail(&self, job_id: &str, token: &str, reason: String) {
        let _ = self.apply(
            job_id,
            Action::Fail {
                token: token.into(),
                error: reason,
            },
        );
    }
}

/// Collect a claimed sync task from `provider`, the built-in source of its
/// request; the canonical evidence of every partition, in plan order.
pub fn collect(host: &Host, provider: &dyn Provider, job: &Value, token: &str) -> Result<Vec<u8>> {
    let job_id = job["id"].as_str().unwrap_or_default();
    if job["kind"] != KIND {
        return Err(CollectError::Failed("任务不是数据同步".into()));
    }
    let payload = &job["payload"];
    let request: SyncRequest = serde_json::from_value(payload["request"].clone())
        .map_err(|_| CollectError::Failed("同步请求格式不受支持".into()))?;
    let manifest = provider.manifest();
    if manifest.id != request.provider || json!(manifest.version) != payload["plugin_version"] {
        return Err(CollectError::Failed(
            "插件版本已变化，请重新提交同步".into(),
        ));
    }
    sync::task_identity(payload).map_err(source_error)?;
    let owner = request
        .connection_id
        .as_deref()
        .unwrap_or(&request.provider);
    let values = configuration::resolve(
        host.credentials,
        &payload["configuration"],
        owner,
        &manifest.configuration,
    )
    .map_err(source_error)?;
    let reused = step(host.store, |tx| {
        observations::resume(
            tx,
            host.artifacts,
            host.tasks,
            job_id,
            token,
            now(),
            host.trace,
        )
        .map_err(evidence_error)
    })?;
    let plan = providers::plan(provider, &request).map_err(CollectError::Failed)?;
    let mut evidence: Vec<Value> = Vec::with_capacity(plan.len());
    for (index, partition) in plan.iter().enumerate() {
        host.progress(job_id, token, index, plan.len())?;
        if let Some(value) = reused.get(&index.to_string()) {
            evidence.push(value.clone());
        } else {
            match provider.fetch(partition, &values) {
                Ok(records) => {
                    let rows: Vec<Value> = records
                        .iter()
                        .map(|record| {
                            let row: Map<String, Value> = partition
                                .fields
                                .iter()
                                .map(|field| {
                                    (
                                        field.clone(),
                                        record.get(field).cloned().unwrap_or(Value::Null),
                                    )
                                })
                                .collect();
                            Value::Object(row)
                        })
                        .collect();
                    evidence.push(json!({
                        "partition": partition, "rows": rows, "observed_at": observed_at(),
                    }));
                }
                Err(message) => {
                    let prefix = message.split([':', '：']).next().unwrap_or_default();
                    let code = if ERROR_CODES.contains(&prefix) {
                        prefix
                    } else {
                        "FETCH_FAILED"
                    };
                    let failure = json!({
                        "partition": partition, "rows": [], "observed_at": observed_at(),
                        "error_code": code,
                    });
                    host.record(job_id, token, index, &failure)?;
                    return Err(CollectError::Failed(format!(
                        "{code}：采集失败，请检查数据源后重试"
                    )));
                }
            }
        }
        host.record(job_id, token, index, &evidence[index])?;
        if canonical(&Value::Array(evidence.clone())).len() > MAX_EVIDENCE {
            return Err(CollectError::Failed(
                "本次同步超过 7.5 MB，请缩小范围".into(),
            ));
        }
        host.progress(job_id, token, index + 1, plan.len())?;
    }
    Ok(canonical(&Value::Array(evidence)))
}
