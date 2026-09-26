//! Lease-fenced source observations of sync tasks. Each partition's response is
//! retained as canonical evidence independently of standard publication, and
//! reused by a resumed task only when every fixed input is identical.
use crate::ids::canonical;
use crate::providers;
use asterion_data_store::minute::{self, Minute};
use asterion_data_store::provider::{Partition, SyncRequest};
use asterion_data_store::table::Row;
use asterion_data_store::types;
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::files;
use asterion_kernel::tasks::repository::{Repository, Request};
use asterion_store::{StoreError, Transaction};
use chrono::{DateTime, FixedOffset, NaiveDate, Timelike};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

pub const KIND: &str = "data.sync";
/// Largest retained evidence of one partition, in canonical bytes.
pub const MAX_BYTES: usize = 8_000_000;
const RESUMED_BYTES: usize = 7_000_000;
const ERROR_CODES: [&str; 5] = [
    "AUTH_FAILED",
    "PERMISSION_DENIED",
    "RATE_LIMITED",
    "PROVIDER_REJECTED",
    "FETCH_FAILED",
];

#[derive(Debug)]
pub enum EvidenceError {
    /// Evidence the task cannot accept, with the user-facing reason.
    Refused(String),
    /// A lease, retry or reuse conflict.
    Conflict(String),
    NotFound,
    Store(StoreError),
}
impl From<StoreError> for EvidenceError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
impl From<String> for EvidenceError {
    fn from(message: String) -> Self {
        Self::Refused(message)
    }
}
impl From<&str> for EvidenceError {
    fn from(message: &str) -> Self {
        Self::Refused(message.into())
    }
}
type Result<T> = std::result::Result<T, EvidenceError>;

fn conflict(message: &str) -> EvidenceError {
    EvidenceError::Conflict(message.into())
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EvidenceSource {
    pub job_id: String,
    pub attempt: i64,
    pub partition_index: i64,
    pub checksum: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawObservation {
    partition: Partition,
    observed_at: String,
    rows: Vec<Map<String, Value>>,
    #[serde(default)]
    error_code: Option<String>,
    #[serde(default)]
    reused_from: Option<EvidenceSource>,
}

/// One partition's response as observed by a worker.
#[derive(Debug, Clone, PartialEq)]
pub struct Observation {
    pub partition: Partition,
    pub observed_at: DateTime<FixedOffset>,
    pub rows: Vec<Map<String, Value>>,
    pub error_code: Option<String>,
    pub reused_from: Option<EvidenceSource>,
}

/// JSON form of an instant: microseconds when present, `Z` for UTC.
fn json_time(at: &DateTime<FixedOffset>) -> String {
    let micros = at.nanosecond() / 1000 % 1_000_000;
    let fraction = if micros == 0 {
        String::new()
    } else {
        format!(".{micros:06}")
    };
    let offset = if at.offset().local_minus_utc() == 0 {
        "Z".to_string()
    } else {
        at.format("%:z").to_string()
    };
    format!("{}{fraction}{offset}", at.format("%Y-%m-%dT%H:%M:%S"))
}

/// ISO form of an instant as recorded in manifests and availability times.
fn iso_time(at: &DateTime<FixedOffset>) -> String {
    let text = json_time(at);
    match text.strip_suffix('Z') {
        Some(prefix) => format!("{prefix}+00:00"),
        None => text,
    }
}

impl Observation {
    pub fn parse(value: &Value) -> Result<Self> {
        let raw: RawObservation = serde_json::from_value(value.clone())
            .map_err(|_| EvidenceError::from("分段证据格式错误"))?;
        let observed_at = DateTime::parse_from_rfc3339(&raw.observed_at)
            .map_err(|_| EvidenceError::from("采集时间证据无效"))?;
        let valid_source = raw
            .reused_from
            .as_ref()
            .is_none_or(|source| source.attempt >= 1 && source.partition_index >= 0);
        if raw.rows.len() > 10_000
            || raw.partition.validate().is_err()
            || !valid_source
            || raw
                .error_code
                .as_deref()
                .is_some_and(|code| !ERROR_CODES.contains(&code))
        {
            return Err("分段证据格式错误".into());
        }
        Ok(Self {
            partition: raw.partition,
            observed_at,
            rows: raw.rows,
            error_code: raw.error_code,
            reused_from: raw.reused_from,
        })
    }

    pub fn to_json(&self) -> Value {
        json!({
            "partition": self.partition,
            "observed_at": json_time(&self.observed_at),
            "rows": self.rows,
            "error_code": self.error_code,
            "reused_from": self.reused_from,
        })
    }

    /// Canonical bytes and their SHA-256 digest.
    pub fn sealed(&self) -> (Vec<u8>, String) {
        let content = canonical(&self.to_json());
        let digest = hex::encode(Sha256::digest(&content));
        (content, digest)
    }
}

/// A JSON column as read back: text on SQLite, a document on PostgreSQL.
fn document(value: Value) -> Value {
    match value {
        Value::String(text) => serde_json::from_str(&text).unwrap_or(Value::Null),
        value => value,
    }
}

fn lease(
    tx: &Transaction,
    tasks: &Repository,
    job_id: &str,
    token: &str,
    now: f64,
    trace: &Value,
) -> Result<Value> {
    let job = tasks
        .run(
            tx.connection(),
            Request::Lease {
                id: job_id.into(),
                token: token.into(),
                now,
            },
            trace.clone(),
        )
        .map_err(|error| EvidenceError::Conflict(error.to_string()))?;
    if job["kind"] != KIND {
        return Err(conflict("Not a data sync job"));
    }
    Ok(job)
}

fn request_of(payload: &Value) -> Result<SyncRequest> {
    serde_json::from_value(payload["request"].clone())
        .map_err(|_| EvidenceError::from("同步请求格式不受支持"))
}

fn saved(tx: &Transaction, job_id: &str, attempt: i64, index: i64) -> Result<Option<Value>> {
    Ok(tx
        .rows(
            "SELECT manifest FROM ingestion_observations WHERE job_id = ? AND attempt = ? AND partition_index = ?",
            vec![json!(job_id), json!(attempt), json!(index)],
        )?
        .into_iter()
        .next()
        .map(|mut row| document(row.remove(0))))
}

fn path(job_id: &str, attempt: i64, index: i64, checksum: &str) -> String {
    format!("sources/{job_id}/{attempt}/{index}/{checksum}.json")
}

fn read(
    store: &ArtifactStore,
    job_id: &str,
    attempt: i64,
    index: i64,
    manifest: &Value,
) -> Result<Observation> {
    let checksum = manifest["checksum"].as_str().unwrap_or_default();
    let content = store
        .read(
            &path(job_id, attempt, index, checksum),
            checksum,
            manifest["bytes"].as_u64(),
        )
        .map_err(|error| match error {
            files::Error::Io(error) if error.kind() == std::io::ErrorKind::NotFound => {
                EvidenceError::from("原始证据文件缺失")
            }
            _ => EvidenceError::from("原始证据校验和不一致"),
        })?;
    let value: Value =
        serde_json::from_slice(&content).map_err(|_| EvidenceError::from("原始证据格式错误"))?;
    Observation::parse(&value)
}

fn day(text: &Option<String>) -> Option<NaiveDate> {
    text.as_deref()
        .and_then(|text| NaiveDate::parse_from_str(text, "%Y-%m-%d").ok())
}

/// The retained evidence of `source`, if this task may reuse it unchanged.
fn reusable(
    tx: &Transaction,
    store: &ArtifactStore,
    job: &Value,
    source: &EvidenceSource,
) -> Result<Observation> {
    let payload = &job["payload"];
    let own = source.job_id == job["id"] && source.attempt < job["attempt"].as_i64().unwrap_or(0);
    if payload["resume_from"].as_str() != Some(source.job_id.as_str()) && !own {
        return Err(conflict(
            "Evidence source is not an authorized resume input",
        ));
    }
    let original = tx
        .rows(
            "SELECT payload FROM jobs WHERE id = ?",
            vec![json!(source.job_id)],
        )?
        .into_iter()
        .next()
        .map(|mut row| document(row.remove(0)))
        .ok_or_else(|| conflict("Resume evidence changed or missing"))?;
    let fixed = |payload: &Value| payload["configuration"]["ref"].as_str().map(str::to_string);
    match (fixed(&original), fixed(payload)) {
        (Some(old), Some(new)) if old == new => {}
        _ => return Err("采集配置缺失或已变化，不能复用分段".into()),
    }
    for key in ["plugin_version", "type_id", "type_schema_version"] {
        if original.get(key).is_none() || original[key] != payload[key] {
            return Err("采集版本不兼容，不能复用".into());
        }
    }
    let mut request = request_of(payload)?;
    let mut old_request = request_of(&original)?;
    old_request.command_id.clone_from(&request.command_id);
    if request != old_request {
        return Err(conflict("Resume request changed"));
    }
    let manifest = saved(tx, &source.job_id, source.attempt, source.partition_index)?
        .filter(|manifest| manifest["checksum"] == source.checksum)
        .ok_or_else(|| conflict("Resume evidence changed or missing"))?;
    let value = read(
        store,
        &source.job_id,
        source.attempt,
        source.partition_index,
        &manifest,
    )?;
    let provider = providers::get(&request.provider)?;
    let type_id = payload["type_id"].as_str().unwrap_or_default();
    let data_type = types::manifest_of(type_id)?;
    if json!(provider.manifest().version) != payload["plugin_version"]
        || json!(data_type.schema_version) != payload["type_schema_version"]
    {
        return Err("采集版本不兼容，不能复用".into());
    }
    let plan = providers::plan(provider.as_ref(), &request)?;
    let index = usize::try_from(source.partition_index).unwrap_or(usize::MAX);
    if plan.get(index) != Some(&value.partition) {
        return Err("采集计划不兼容，不能复用".into());
    }
    if value.error_code.is_some()
        || value.rows.is_empty()
        || value.rows.len() as u64 >= value.partition.limit
    {
        return Err("分段没有可复用的完整响应".into());
    }
    if request.start.is_some() {
        let (Some(start), Some(end)) = (day(&value.partition.start), day(&value.partition.end))
        else {
            return Err("采集分段日期缺失".into());
        };
        request.start = Some(start);
        request.end = Some(end);
    }
    let records: Vec<Value> = value.rows.iter().cloned().map(Value::Object).collect();
    let mut rows: Vec<Row> = provider.normalize(&request, &records)?;
    if type_id == minute::TYPE {
        Minute::from_value(&payload["minute_context"])?
            .bind_rows(&mut rows, &iso_time(&value.observed_at))?;
    }
    let rows: Vec<Value> = rows
        .iter()
        .map(|row| serde_json::to_value(row).expect("serializable row"))
        .collect();
    types::validate(type_id, &rows)?;
    types::coverage(type_id, rows.len(), request.start, request.end)?;
    Ok(Observation {
        reused_from: Some(source.clone()),
        ..value
    })
}

/// Retain one partition's observation for the task's current attempt.
#[allow(clippy::too_many_arguments)]
pub fn record(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &Repository,
    job_id: &str,
    token: &str,
    index: i64,
    value: &Observation,
    now: f64,
    trace: &Value,
) -> Result<Value> {
    let (content, digest) = value.sealed();
    if content.len() > MAX_BYTES {
        return Err("分段证据超过 8 MB".into());
    }
    let job = lease(tx, tasks, job_id, token, now, trace)?;
    let request = request_of(&job["payload"])?;
    let provider = providers::get(&request.provider)?;
    if json!(provider.manifest().version) != job["payload"]["plugin_version"] {
        return Err(conflict("Provider version changed"));
    }
    let plan = providers::plan(provider.as_ref(), &request)?;
    let position = usize::try_from(index).unwrap_or(usize::MAX);
    if plan.get(position) != Some(&value.partition) {
        return Err("分段证据与采集计划不一致".into());
    }
    if let Some(source) = &value.reused_from
        && (source.partition_index != index || &reusable(tx, store, &job, source)? != value)
    {
        return Err(conflict("Reused observation changed"));
    }
    let observed = value.observed_at.timestamp_micros() as f64 / 1e6;
    let created = job["created_at"].as_f64().unwrap_or(f64::MAX);
    if !((value.reused_from.is_some() || created <= observed) && observed <= now + 5.0) {
        return Err("采集时间证据无效".into());
    }
    if value.error_code.is_some() && !value.rows.is_empty() {
        return Err("失败证据不能包含成功响应".into());
    }
    let declared: BTreeSet<&String> = value.partition.fields.iter().collect();
    if value
        .rows
        .iter()
        .any(|row| row.keys().collect::<BTreeSet<_>>() != declared)
    {
        return Err("采集字段与插件声明不一致".into());
    }
    let attempt = job["attempt"].as_i64().unwrap_or(0);
    if let Some(old) = saved(tx, job_id, attempt, index)? {
        if old["checksum"] != digest {
            return Err(conflict("Observation retry changed content"));
        }
        return Ok(old);
    }
    let name = path(job_id, attempt, index, &digest);
    let status = value.error_code.clone().unwrap_or_else(|| {
        if value.rows.is_empty() {
            "EMPTY_UNCONFIRMED"
        } else {
            "RECEIVED"
        }
        .into()
    });
    let manifest = json!({
        "checksum": digest,
        "uri": format!("asterion://local/{name}"),
        "bytes": content.len(),
        "rows": value.rows.len(),
        "partition": value.partition,
        "observed_at": iso_time(&value.observed_at),
        "status": status,
        "plugin_version": job["payload"]["plugin_version"],
        "reused_from": value.reused_from,
    });
    store
        .put(&name, &content)
        .map_err(|_| EvidenceError::from("无法保存原始证据"))?;
    tx.execute(
        "INSERT INTO ingestion_observations (job_id, attempt, partition_index, manifest) VALUES (?, ?, ?, ?)",
        vec![json!(job_id), json!(attempt), json!(index), json!(manifest.to_string())],
    )?;
    Ok(manifest)
}

/// Retain every reusable earlier observation for the current attempt: first
/// this task's latest attempts, then the task it resumes; the rest is fetched.
pub fn resume(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &Repository,
    job_id: &str,
    token: &str,
    now: f64,
    trace: &Value,
) -> Result<BTreeMap<String, Value>> {
    let job = lease(tx, tasks, job_id, token, now, trace)?;
    let attempt = job["attempt"].as_i64().unwrap_or(0);
    let resumed = job["payload"]["resume_from"].as_str().unwrap_or_default();
    let mut candidates: Vec<(String, i64, i64, Value)> = tx
        .rows(
            "SELECT job_id, attempt, partition_index, manifest FROM ingestion_observations \
             WHERE (job_id = ? AND attempt < ?) OR job_id = ?",
            vec![json!(job_id), json!(attempt), json!(resumed)],
        )?
        .into_iter()
        .map(|row| {
            (
                row[0].as_str().unwrap_or_default().to_string(),
                row[1].as_i64().unwrap_or_default(),
                row[2].as_i64().unwrap_or_default(),
                document(row[3].clone()),
            )
        })
        .collect();
    candidates.sort_by(|a, b| (b.0 == job_id).cmp(&(a.0 == job_id)).then(b.1.cmp(&a.1)));
    let mut reused: BTreeMap<i64, Observation> = BTreeMap::new();
    let mut retained = 0;
    for (source_job, source_attempt, index, manifest) in candidates {
        if reused.contains_key(&index) {
            continue;
        }
        let source = EvidenceSource {
            job_id: source_job,
            attempt: source_attempt,
            partition_index: index,
            checksum: manifest["checksum"]
                .as_str()
                .unwrap_or_default()
                .to_string(),
        };
        // Missing, corrupt, empty or invalid partitions are fetched again.
        match reusable(tx, store, &job, &source) {
            Ok(value) => {
                let size = value.sealed().0.len();
                if retained + size <= RESUMED_BYTES {
                    retained += size;
                    reused.insert(index, value);
                }
            }
            Err(EvidenceError::Store(error)) => return Err(EvidenceError::Store(error)),
            Err(_) => {}
        }
    }
    let mut result = BTreeMap::new();
    for (index, value) in reused {
        record(tx, store, tasks, job_id, token, index, &value, now, trace)?;
        result.insert(index.to_string(), value.to_json());
    }
    Ok(result)
}

/// A publication must carry exactly the observations retained for its attempt.
pub fn verify_publication(
    tx: &Transaction,
    job_id: &str,
    attempt: i64,
    envelope: &[Value],
) -> Result<()> {
    let saved: Vec<(i64, Value)> = tx
        .rows(
            "SELECT partition_index, manifest FROM ingestion_observations WHERE job_id = ? AND attempt = ? ORDER BY partition_index",
            vec![json!(job_id), json!(attempt)],
        )?
        .into_iter()
        .map(|row| (row[0].as_i64().unwrap_or(-1), document(row[1].clone())))
        .collect();
    if saved.is_empty() {
        let reused = |value: &Value| !matches!(value.get("reused_from"), None | Some(Value::Null));
        if envelope.iter().any(reused) {
            return Err(conflict(
                "Reused evidence must be retained before publication",
            ));
        }
        return Ok(());
    }
    if saved.len() != envelope.len() {
        return Err(conflict("Publication is missing retained observations"));
    }
    for (position, ((index, manifest), value)) in saved.iter().zip(envelope).enumerate() {
        let (_, digest) = Observation::parse(value)?.sealed();
        if *index != position as i64 || manifest["checksum"] != digest {
            return Err(conflict("Publication changed retained observation"));
        }
    }
    Ok(())
}

/// Retained observations of a sync task, oldest attempt first.
pub fn list(tx: &Transaction, job_id: &str, offset: i64, limit: i64) -> Result<Value> {
    let known = tx.rows(
        "SELECT id FROM jobs WHERE id = ? AND kind = ?",
        vec![json!(job_id), json!(KIND)],
    )?;
    if known.is_empty() {
        return Err(EvidenceError::NotFound);
    }
    let total = tx.rows(
        "SELECT COUNT(*) FROM ingestion_observations WHERE job_id = ?",
        vec![json!(job_id)],
    )?[0][0]
        .as_i64()
        .unwrap_or_default();
    let items: Vec<Value> = tx
        .rows(
            "SELECT job_id, attempt, partition_index, manifest FROM ingestion_observations \
             WHERE job_id = ? ORDER BY attempt, partition_index LIMIT ? OFFSET ?",
            vec![json!(job_id), json!(limit), json!(offset)],
        )?
        .into_iter()
        .map(|row| {
            json!({"job_id": row[0], "attempt": row[1], "partition_index": row[2],
                   "manifest": document(row[3].clone())})
        })
        .collect();
    Ok(json!({"items": items, "total": total}))
}

/// Rows of one retained observation, read back verified.
pub fn preview(
    tx: &Transaction,
    store: &ArtifactStore,
    job_id: &str,
    attempt: i64,
    index: i64,
    offset: usize,
    limit: usize,
) -> Result<Value> {
    let manifest = saved(tx, job_id, attempt, index)?.ok_or(EvidenceError::NotFound)?;
    let value = read(store, job_id, attempt, index, &manifest)?;
    let rows: Vec<&Map<String, Value>> = value.rows.iter().skip(offset).take(limit).collect();
    Ok(json!({"manifest": manifest, "rows": rows, "total": value.rows.len()}))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Evidence retained before this service existed keeps its digests.
    #[test]
    fn canonical_evidence_matches_existing_digests() {
        let value = json!({
            "partition": {"api": "fut_daily", "params": {"ts_code": "RB2610.SHF", "exchange": "SHFE"},
                          "fields": ["ts_code", "close", "name"], "limit": 2000,
                          "start": "2024-01-02", "end": "2024-01-02"},
            "observed_at": "2024-01-02T09:00:00.123000+00:00",
            "rows": [{"ts_code": "RB2610.SHF", "close": 3210.5, "name": "螺纹\u{2028}\"钢\""},
                     {"ts_code": "RB2610.SHF", "close": 1e-05, "name": null}],
            "reused_from": {"job_id": "j", "attempt": 1, "partition_index": 0, "checksum": "c"},
        });
        let observation = Observation::parse(&value).unwrap();
        let (content, digest) = observation.sealed();
        assert!(
            String::from_utf8(content)
                .unwrap()
                .contains(r#""observed_at":"2024-01-02T09:00:00.123000Z""#)
        );
        assert_eq!(
            digest,
            "7664f576fa4d53095bc8f8c5ac272bb359a85dddd45545df8afa7346537c314c"
        );
        assert_eq!(
            iso_time(&observation.observed_at),
            "2024-01-02T09:00:00.123000+00:00"
        );
        let local = DateTime::parse_from_rfc3339("2024-01-02T09:00:00+08:00").unwrap();
        assert_eq!(json_time(&local), "2024-01-02T09:00:00+08:00");
    }

    #[test]
    fn malformed_observations_are_refused() {
        let base = json!({
            "partition": {"api": "a", "params": {}, "fields": ["x"], "limit": 10},
            "observed_at": "2024-01-02T09:00:00Z", "rows": [],
        });
        assert!(Observation::parse(&base).is_ok());
        let changed = |key: &str, value: Value| {
            let mut changed = base.clone();
            changed[key] = value;
            changed
        };
        for invalid in [
            changed("observed_at", json!("2024-01-02T09:00:00")),
            changed("error_code", json!("UNKNOWN")),
            changed("token", json!("secret")),
            changed("rows", json!(vec![json!({}); 10_001])),
            changed(
                "reused_from",
                json!({"job_id": "j", "attempt": 0, "partition_index": 0, "checksum": "c"}),
            ),
        ] {
            assert!(Observation::parse(&invalid).is_err());
        }
    }
}
