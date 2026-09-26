//! Admission and submission of source sync tasks. A task fixes its request,
//! source and data type versions, configuration snapshot and, for
//! contract-bound types, the identity catalog of one actual contract.
use crate::configuration::{Result, SourceError};
use crate::connections;
use crate::credentials::Credentials;
use crate::preview::{PreviewError, preview};
use crate::providers;
use asterion_data_store::identity::{source_catalog, validate_catalog_input};
use asterion_data_store::minute::{self, Minute};
use asterion_data_store::provider::{Provider, SyncRequest};
use asterion_data_store::types;
use asterion_instrument_catalog::{SourceIdentity, SourceResolver, catalog_id};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::tasks::Action;
use asterion_kernel::tasks::repository::{Repository, Request};
use asterion_store::Transaction;
use chrono::NaiveDate;
use serde::Deserialize;
use serde_json::{Map, Value, json};

pub const KIND: &str = "data.sync";
/// Source types whose rows belong to one fixed actual contract lifecycle.
pub const IDENTITY_TYPES: [&str; 3] = ["futures.daily", "futures.settlement", "futures.minute"];

/// A sync request as the workbench submits it.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Submission {
    pub command_id: String,
    pub provider: String,
    #[serde(default)]
    pub connection_id: Option<String>,
    pub dataset: String,
    pub exchange: String,
    #[serde(default)]
    pub symbol: String,
    #[serde(default)]
    pub frequency: Option<String>,
    #[serde(default)]
    pub window: Option<asterion_data_store::provider::SourceWindow>,
    #[serde(default)]
    pub start: Option<NaiveDate>,
    #[serde(default)]
    pub end: Option<NaiveDate>,
    #[serde(default)]
    pub minute_context: Option<Value>,
    #[serde(default)]
    pub contracts_version_id: Option<String>,
}
impl Submission {
    pub fn request(&self) -> SyncRequest {
        SyncRequest {
            command_id: self.command_id.clone(),
            provider: self.provider.clone(),
            connection_id: self.connection_id.clone(),
            dataset: self.dataset.clone(),
            exchange: self.exchange.clone(),
            symbol: self.symbol.clone(),
            frequency: self.frequency.clone(),
            window: self.window.clone(),
            start: self.start,
            end: self.end,
        }
    }
    pub fn valid(&self, today: NaiveDate) -> bool {
        self.request().validate(today).is_ok()
            && self
                .contracts_version_id
                .as_ref()
                .is_none_or(|id| (1..=100).contains(&id.chars().count()))
    }
}

/// Inputs a task is admitted with besides its request.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Admission {
    #[serde(default)]
    pub resume_from: Option<String>,
    #[serde(default)]
    pub coverage_report_id: Option<String>,
    #[serde(default)]
    pub retry_of: Option<String>,
    #[serde(default)]
    pub preparation_id: Option<String>,
    #[serde(default)]
    pub contract_identity: Option<Value>,
    #[serde(default)]
    pub minute_context: Option<Value>,
}

fn capability_type(provider: &dyn Provider, request: &SyncRequest) -> Result<String> {
    provider
        .manifest()
        .capabilities
        .into_iter()
        .find(|capability| capability.id == request.dataset)
        .map(|capability| capability.type_id)
        .ok_or_else(|| "所选数据类型不可用".into())
}

fn contract_of(request: &SyncRequest) -> String {
    format!(
        "{}.{}",
        request.exchange,
        request.symbol.split('.').next().unwrap_or_default()
    )
}

/// The identity catalog of a request's source symbol in a fixed contracts version.
fn identity_catalog(
    tx: &Transaction,
    store: &ArtifactStore,
    version_id: &str,
    symbol: &str,
) -> Result<asterion_instrument_catalog::ReferenceCatalog> {
    let read = preview(tx, store, version_id, 0, 10_001).map_err(|error| match error {
        PreviewError::NotFound => SourceError::from("合约资料版本不存在，请先同步合约资料"),
        PreviewError::Refused(message) => SourceError::Refused(message),
        PreviewError::Store(error) => SourceError::Store(error),
    })?;
    let value = serde_json::to_value(&read).expect("serializable preview");
    Ok(source_catalog(&value, version_id, &[symbol.to_string()])?)
}

/// Minute tasks fix one trading day's sessions and source window of one
/// actual contract; other tasks accept no minute inputs.
fn minute_context(
    request: &SyncRequest,
    type_id: &str,
    context: &Value,
    identity: Option<&SourceResolver>,
) -> Result<()> {
    if type_id != minute::TYPE {
        if !context.is_null() || request.window.is_some() || request.frequency.is_some() {
            return Err("非分钟任务不接受分钟周期、时段与时间窗口".into());
        }
        return Ok(());
    }
    let minute = Minute::from_value(context)?;
    if request.frequency.as_deref() != Some(minute.context.frequency.as_str()) {
        return Err("分钟请求周期与固定依据不一致".into());
    }
    let (Some(start), Some(identity)) = (request.start, identity) else {
        return Err("分钟同步必须固定一个实际合约和一个交易日".into());
    };
    if request.end != Some(start) {
        return Err("分钟同步必须固定一个实际合约和一个交易日".into());
    }
    let actual = identity.resolve(start)?;
    let spec = &minute.context.trading_time.spec;
    if actual.product_id != format!("{}.{}", spec.exchange.as_str(), spec.product) {
        return Err("分钟时段与实际合约品种不一致".into());
    }
    if request.window.as_ref() != Some(&minute.window(&contract_of(request), start)?) {
        return Err("分钟来源时间窗口与固定交易时段不一致".into());
    }
    Ok(())
}

/// Check a task's request, type and fixed identity against its source; the
/// resolver of contract-bound tasks.
pub fn task_identity(payload: &Value) -> Result<Option<SourceResolver>> {
    let request: SyncRequest = serde_json::from_value(payload["request"].clone())
        .map_err(|_| SourceError::from("同步请求格式不受支持"))?;
    let provider = providers::get(&request.provider)?;
    providers::plan(provider.as_ref(), &request)?;
    let type_id = capability_type(provider.as_ref(), &request)?;
    if payload["type_id"] != type_id.as_str() {
        return Err("同步任务的数据类型与供应商能力不一致".into());
    }
    let identity = &payload["contract_identity"];
    if !IDENTITY_TYPES.contains(&type_id.as_str()) {
        minute_context(&request, &type_id, &payload["minute_context"], None)?;
        if !identity.is_null() {
            return Err("此同步数据类型不接受合约身份依据".into());
        }
        return Ok(None);
    }
    let resolver = SourceResolver::from_value(identity.clone())?;
    let model = resolver.model();
    if model.source != request.provider || model.symbol != request.symbol {
        return Err("同步来源代码与固定合约身份不一致".into());
    }
    let (Some(start), Some(end)) = (request.start, request.end) else {
        return Err("合约数据同步须明确起止交易日".into());
    };
    let (first, last) = (resolver.resolve(start)?, resolver.resolve(end)?);
    if first.id != last.id {
        return Err("一次同步不能跨越多个实际合约生命周期".into());
    }
    if !first
        .product_id
        .starts_with(&format!("{}.", request.exchange))
    {
        return Err("固定合约身份与请求交易所不一致".into());
    }
    minute_context(
        &request,
        &type_id,
        &payload["minute_context"],
        Some(&resolver),
    )?;
    Ok(Some(resolver))
}

/// A task's fixed identity must be rebuilt unchanged from its fixed contracts
/// version, which stays the connection's current published standard version.
pub fn validate_identity(
    tx: &Transaction,
    store: &ArtifactStore,
    payload: &Value,
) -> Result<SourceResolver> {
    let resolver = SourceResolver::from_value(payload["contract_identity"].clone())?;
    let request: SyncRequest = serde_json::from_value(payload["request"].clone())
        .map_err(|_| SourceError::from("同步请求格式不受支持"))?;
    let identity = resolver.model();
    if identity.source != request.provider || identity.symbol != request.symbol {
        return Err("同步的来源代码与固定身份不一致".into());
    }
    for item in &identity.catalog.inputs {
        let manifest = tx
            .rows(
                "SELECT manifest FROM data_versions WHERE id = ?",
                vec![json!(item.version_id)],
            )?
            .into_iter()
            .next()
            .map(|mut row| match row.remove(0) {
                Value::String(text) => serde_json::from_str(&text).unwrap_or(Value::Null),
                value => value,
            })
            .ok_or("同步的固定资料缺失")?;
        validate_catalog_input(item, &manifest)?;
        if manifest["scope"]["connection_id"].as_str() != request.connection_id.as_deref() {
            return Err("同步的固定资料连接不一致".into());
        }
    }
    let version_id = &identity.catalog.inputs[0].version_id;
    if identity_catalog(tx, store, version_id, &request.symbol)? != identity.catalog {
        return Err("固定身份目录与资料文件内容不一致".into());
    }
    Ok(resolver)
}

/// A request's source, data type and identity for a new task: minute tasks
/// get their window and frequency from the fixed sessions; contract-bound
/// tasks the identity of their symbol in the chosen contracts version.
pub fn prepare(
    tx: &Transaction,
    store: &ArtifactStore,
    submission: &Submission,
) -> Result<(SyncRequest, Option<Value>)> {
    let mut request = submission.request();
    let provider = providers::get(&request.provider)?;
    let capability = provider
        .manifest()
        .capabilities
        .into_iter()
        .find(|capability| capability.id == request.dataset)
        .ok_or("所选数据类型不可用")?;
    let context = submission.minute_context.clone().unwrap_or(Value::Null);
    if capability.type_id == minute::TYPE {
        let (Some(start), false) = (request.start, context.is_null()) else {
            return Err("分钟同步须选择单个交易日、固定交易时段和时间戳含义".into());
        };
        if request.end != Some(start) {
            return Err("分钟同步须选择单个交易日、固定交易时段和时间戳含义".into());
        }
        let minute = Minute::from_value(&context)?;
        if !capability.frequencies.contains(&minute.context.frequency) {
            return Err("来源不支持所选分钟周期".into());
        }
        if request.window.is_some() || request.frequency.is_some() {
            return Err("分钟周期与窗口由分钟上下文生成，不能重复指定".into());
        }
        request.window = Some(minute.window(&contract_of(&request), start)?);
        request.frequency = Some(minute.context.frequency.clone());
    } else if !context.is_null() || request.window.is_some() || request.frequency.is_some() {
        return Err("此类型不接受分钟上下文".into());
    }
    providers::plan(provider.as_ref(), &request)?;
    if !IDENTITY_TYPES.contains(&capability.type_id.as_str()) {
        if submission.contracts_version_id.is_some() {
            return Err("此数据类型不接受合约身份依据".into());
        }
        return Ok((request, None));
    }
    let version_id = submission
        .contracts_version_id
        .as_deref()
        .ok_or("合约数据同步须选择固定合约资料版本")?;
    let catalog = identity_catalog(tx, store, version_id, &request.symbol)?;
    let information_at = catalog
        .contracts
        .iter()
        .map(|contract| contract.provenance.available_at)
        .max()
        .ok_or("固定合约资料中没有所选合约")?;
    let identity = SourceIdentity {
        catalog_id: catalog_id(&catalog)?,
        catalog,
        source: request.provider.clone(),
        symbol: request.symbol.clone(),
        information_at,
    };
    Ok((
        request,
        Some(serde_json::to_value(identity).expect("serializable identity")),
    ))
}

/// The fixed inputs of a new task of `request`: source and type versions and
/// the configuration of an enabled connection.
pub fn payload(
    tx: &Transaction,
    credentials: &Credentials,
    request: &SyncRequest,
) -> Result<Map<String, Value>> {
    let provider = providers::get(&request.provider)?;
    providers::plan(provider.as_ref(), request)?;
    let type_id = capability_type(provider.as_ref(), request)?;
    let definition = types::manifest_of(&type_id)?;
    let owner = request
        .connection_id
        .as_deref()
        .unwrap_or(&request.provider);
    let fixed = connections::fix_for_task(
        tx,
        credentials,
        &providers::manifests(),
        owner,
        &request.provider,
    )?;
    let mut payload = Map::new();
    payload.insert(
        "request".into(),
        serde_json::to_value(request).expect("serializable request"),
    );
    payload.insert("plugin_version".into(), json!(provider.manifest().version));
    payload.insert("type_id".into(), json!(definition.id));
    payload.insert(
        "type_schema_version".into(),
        json!(definition.schema_version),
    );
    payload.extend(fixed);
    Ok(payload)
}

fn tasks_error(error: impl ToString) -> SourceError {
    SourceError::Conflict(error.to_string())
}

fn stored(value: Value) -> Value {
    match value {
        Value::String(text) => serde_json::from_str(&text).unwrap_or(Value::Null),
        value => value,
    }
}

/// The complete task row, as its submitter sees it.
pub fn job_row(tx: &Transaction, id: &str) -> Result<Value> {
    let columns = [
        "id",
        "command_id",
        "kind",
        "payload",
        "state",
        "attempt",
        "token",
        "worker_id",
        "lease_until",
        "created_at",
        "error",
        "result",
    ];
    let row = tx
        .rows(
            &format!("SELECT {} FROM jobs WHERE id = ?", columns.join(", ")),
            vec![json!(id)],
        )?
        .into_iter()
        .next()
        .ok_or_else(|| SourceError::Conflict("Task disappeared after submission".into()))?;
    Ok(Value::Object(
        columns
            .iter()
            .zip(row)
            .map(|(name, value)| {
                let value = if matches!(*name, "payload" | "result") {
                    stored(value)
                } else {
                    value
                };
                ((*name).to_string(), value)
            })
            .collect(),
    ))
}

/// The public view of a task: no payload or lease token.
pub fn public(job: &Value) -> Value {
    let mut view = job.clone();
    if let Some(fields) = view.as_object_mut() {
        fields.remove("payload");
        fields.remove("token");
    }
    view
}

/// The task already accepted for this command, if it has the same inputs.
fn existing(
    tx: &Transaction,
    request: &SyncRequest,
    admission: &Admission,
) -> Result<Option<String>> {
    let Some(row) = tx
        .rows(
            "SELECT id, kind, payload FROM jobs WHERE command_id = ?",
            vec![json!(request.command_id)],
        )?
        .into_iter()
        .next()
    else {
        return Ok(None);
    };
    let payload = stored(row[2].clone());
    let same_request = serde_json::from_value::<SyncRequest>(payload["request"].clone())
        .is_ok_and(|old| &old == request);
    let text = |value: &Option<String>| value.as_ref().map_or(Value::Null, |text| json!(text));
    let value = |value: &Option<Value>| value.clone().unwrap_or(Value::Null);
    let field = |name: &str| payload.get(name).cloned().unwrap_or(Value::Null);
    if row[1] != KIND
        || !same_request
        || field("resume_from") != text(&admission.resume_from)
        || field("coverage_report_id") != text(&admission.coverage_report_id)
        || field("retry_of") != text(&admission.retry_of)
        || field("preparation_id") != text(&admission.preparation_id)
        || field("minute_context") != value(&admission.minute_context)
        || field("contract_identity") != value(&admission.contract_identity)
    {
        return Err(SourceError::Conflict(
            "command_id reused with different input".into(),
        ));
    }
    Ok(Some(row[0].as_str().unwrap_or_default().to_string()))
}

/// Submit a sync task; the same command with the same inputs returns the
/// task already accepted, even when current settings changed since.
#[allow(clippy::too_many_arguments)]
pub fn submit(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &Repository,
    credentials: &Credentials,
    request: &SyncRequest,
    admission: &Admission,
    now: f64,
    trace: &Value,
) -> Result<Value> {
    let provider = providers::get(&request.provider)?;
    providers::plan(provider.as_ref(), request)?;
    let type_id = capability_type(provider.as_ref(), request)?;
    let admitted = json!({
        "request": request,
        "type_id": type_id,
        "contract_identity": admission.contract_identity,
        "minute_context": admission.minute_context,
    });
    if task_identity(&admitted)?.is_some() {
        validate_identity(tx, store, &admitted)?;
    }
    if let Some(id) = existing(tx, request, admission)? {
        return job_row(tx, &id);
    }
    let mut record = payload(tx, credentials, request)?;
    let optional = [
        ("minute_context", admission.minute_context.clone()),
        ("contract_identity", admission.contract_identity.clone()),
        (
            "resume_from",
            admission.resume_from.clone().map(Value::from),
        ),
        (
            "coverage_report_id",
            admission.coverage_report_id.clone().map(Value::from),
        ),
        ("retry_of", admission.retry_of.clone().map(Value::from)),
        (
            "preparation_id",
            admission.preparation_id.clone().map(Value::from),
        ),
    ];
    for (name, value) in optional {
        if let Some(value) = value.filter(|value| !value.is_null()) {
            record.insert(name.into(), value);
        }
    }
    let submitted = tasks.run(
        tx.connection(),
        Request::Submit {
            record: serde_json::from_value(json!({
                "id": uuid::Uuid::new_v4().to_string(),
                "command_id": request.command_id,
                "kind": KIND,
                "payload": record,
                "now": now,
            }))
            .expect("task record"),
        },
        trace.clone(),
    );
    match submitted {
        Ok(job) => job_row(tx, job["id"].as_str().unwrap_or_default()),
        // A concurrent submission of the same command won; return it when
        // its inputs are the same.
        Err(error) => match existing(tx, request, admission)? {
            Some(id) => job_row(tx, &id),
            None => Err(tasks_error(error)),
        },
    }
}

/// Admit and submit a workbench request.
#[allow(clippy::too_many_arguments)]
pub fn admit(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &Repository,
    credentials: &Credentials,
    submission: &Submission,
    now: f64,
    trace: &Value,
) -> Result<Value> {
    let (request, identity) = prepare(tx, store, submission)?;
    let admission = Admission {
        contract_identity: identity,
        minute_context: submission
            .minute_context
            .clone()
            .filter(|value| !value.is_null()),
        ..Admission::default()
    };
    submit(
        tx,
        store,
        tasks,
        credentials,
        &request,
        &admission,
        now,
        trace,
    )
}

/// Daily sync of several symbols of one trading day, all admitted before any
/// task is inserted.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DailyBatch {
    pub command_prefix: String,
    pub provider: String,
    pub connection_id: Option<String>,
    pub exchange: String,
    pub contracts_version_id: String,
    pub trading_day: NaiveDate,
    pub symbols: Vec<String>,
}

#[allow(clippy::too_many_arguments)]
pub fn submit_batch(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &Repository,
    credentials: &Credentials,
    batch: &DailyBatch,
    now: f64,
    trace: &Value,
) -> Result<Value> {
    let mut symbols = batch.symbols.clone();
    symbols.sort();
    symbols.dedup();
    if symbols.len() != batch.symbols.len()
        || !(1..=1000).contains(&symbols.len())
        || !(1..=80).contains(&batch.command_prefix.chars().count())
    {
        return Err("批量日线候选重复或数量无效".into());
    }
    let provider = providers::get(&batch.provider)?;
    let daily: Vec<_> = provider
        .manifest()
        .capabilities
        .into_iter()
        .filter(|capability| capability.type_id == "futures.daily")
        .collect();
    let [capability] = daily.as_slice() else {
        return Err("数据源必须声明唯一日线采集能力".into());
    };
    let mut records = Vec::new();
    for (index, symbol) in symbols.iter().enumerate() {
        let submission = Submission {
            command_id: format!("{}:{index}", batch.command_prefix),
            provider: batch.provider.clone(),
            connection_id: batch.connection_id.clone(),
            dataset: capability.id.clone(),
            exchange: batch.exchange.clone(),
            symbol: symbol.clone(),
            frequency: None,
            window: None,
            start: Some(batch.trading_day),
            end: Some(batch.trading_day),
            minute_context: None,
            contracts_version_id: Some(batch.contracts_version_id.clone()),
        };
        let (request, identity) = prepare(tx, store, &submission)?;
        let mut record = payload(tx, credentials, &request)?;
        record.insert("contract_identity".into(), identity.unwrap_or(Value::Null));
        validate_identity(tx, store, &Value::Object(record.clone()))?;
        records.push(json!({
            "id": uuid::Uuid::new_v4().to_string(),
            "command_id": request.command_id,
            "kind": KIND,
            "payload": record,
            "now": now,
        }));
    }
    let submitted = tasks
        .run(
            tx.connection(),
            serde_json::from_value(json!({"op": "submit_batch", "records": records}))
                .expect("batch"),
            trace.clone(),
        )
        .map_err(tasks_error)?;
    submitted
        .as_array()
        .into_iter()
        .flatten()
        .map(|job| job_row(tx, job["id"].as_str().unwrap_or_default()))
        .collect::<Result<Vec<_>>>()
        .map(Value::Array)
}

/// Record a worker's partition progress under its lease.
#[allow(clippy::too_many_arguments)]
pub fn progress(
    tx: &Transaction,
    tasks: &Repository,
    job_id: &str,
    token: &str,
    completed: i64,
    total: i64,
    now: f64,
    trace: &Value,
) -> Result<()> {
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
        .map_err(tasks_error)?;
    if job["kind"] != KIND {
        return Err(SourceError::Conflict("Not a data sync job".into()));
    }
    let request: SyncRequest = serde_json::from_value(job["payload"]["request"].clone())
        .map_err(|_| SourceError::from("同步请求格式不受支持"))?;
    let provider = providers::get(&request.provider)?;
    let count = providers::plan(provider.as_ref(), &request)?.len() as i64;
    if total != count || !(0..=total).contains(&completed) {
        return Err("Invalid partition progress".into());
    }
    let result = json!({"completed": completed, "total": total})
        .as_object()
        .cloned()
        .expect("object");
    tasks
        .run(
            tx.connection(),
            Request::Apply {
                id: job_id.into(),
                action: Action::Progress {
                    token: token.into(),
                    result,
                },
                now,
            },
            trace.clone(),
        )
        .map_err(tasks_error)?;
    Ok(())
}
