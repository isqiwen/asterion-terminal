//! CSV imports into the fixed data types: strict parsing and field mapping,
//! trading-time and identity checks, standard and chart tables, and the
//! publication of an import task's versions in its lease-fenced transaction.
use crate::ids::stable_id;
use crate::preview::{PreviewError, preview};
use asterion_data_store::bars::{daily_chart, encode_bars};
use asterion_data_store::pyfmt::{decimal_text, iso_date};
use asterion_data_store::table::{Row, encode_table};
use asterion_data_store::types;
use asterion_instrument_catalog::ImportResolver;
use asterion_kernel::artifacts::{ArtifactRef, ArtifactStore};
use asterion_store::Transaction;
use asterion_trading_calendar::{BarBoundary, Boundary, Calendar, TimeVersion, Validate};
use chrono::{DateTime, FixedOffset, NaiveDate, Utc};
use regex::Regex;
use serde::Deserialize;
use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::sync::LazyLock;

pub type Result<T> = std::result::Result<T, String>;

pub const KIND: &str = "data.import_csv";
const LIMIT: usize = 2_000_000;
const BAR_FIELDS: [&str; 9] = [
    "contract",
    "event_time",
    "available_at",
    "trading_day",
    "open",
    "high",
    "low",
    "close",
    "volume",
];
const DAILY_FIELDS: [&str; 10] = [
    "contract",
    "trading_day",
    "open",
    "high",
    "low",
    "close",
    "vol",
    "amount",
    "oi",
    "settle",
];

static SOURCE_ID: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^[a-z][a-z0-9_]{0,40}$").expect("pattern"));
static DAILY_CONTRACT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^(?:(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}|SIM\.DEMO001)$")
        .expect("pattern")
});

fn default_type() -> String {
    "futures.bars".into()
}
fn default_frequency() -> String {
    "unspecified".into()
}

/// The request form of import options, as the published contract declares.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RawOptions {
    pub identity: Value,
    #[serde(default)]
    pub trading_time: Option<Value>,
    #[serde(default)]
    pub timestamp_semantics: Option<String>,
    #[serde(default = "default_type")]
    pub type_id: String,
    #[serde(default = "default_frequency")]
    pub frequency: String,
    pub source_id: String,
    #[serde(default)]
    pub column_mapping: Row,
}

/// Validated import options.
pub struct ImportOptions {
    raw: RawOptions,
    identity: ImportResolver,
    calendar: Option<Calendar>,
}

impl ImportOptions {
    pub fn new(raw: RawOptions) -> Result<Self> {
        let identity = ImportResolver::from_value(raw.identity.clone())?;
        let calendar = match &raw.trading_time {
            None | Some(Value::Null) => None,
            Some(value) => {
                let version: TimeVersion =
                    serde_json::from_value(value.clone()).map_err(|e| e.to_string())?;
                version.validate()?;
                Some(Calendar::new(version.spec)?)
            }
        };
        if !matches!(
            raw.timestamp_semantics.as_deref(),
            None | Some("bar_start" | "bar_end")
        ) || !matches!(raw.type_id.as_str(), "futures.bars" | "futures.daily")
            || !matches!(
                raw.frequency.as_str(),
                "unspecified" | "1m" | "5m" | "15m" | "30m" | "1h" | "1d"
            )
            || !SOURCE_ID.is_match(&raw.source_id)
            || raw.column_mapping.0.len() > 20
            || raw.column_mapping.0.iter().any(|(_, v)| !v.is_string())
        {
            return Err("导入选项不符合当前契约".into());
        }
        let bars = raw.type_id == "futures.bars";
        if bars
            && (calendar.is_none()
                || raw.timestamp_semantics.is_none()
                || raw.frequency == "unspecified")
        {
            return Err("日内行情必须选择交易时间版本、明确频率及时间戳起止口径".into());
        }
        if raw.type_id == "futures.daily" && raw.frequency != "1d" {
            return Err("日线频率必须为 1d".into());
        }
        if bars && raw.frequency == "1d" {
            return Err("日线请使用期货日线类型".into());
        }
        Ok(Self {
            raw,
            identity,
            calendar,
        })
    }
    pub fn from_value(value: &Value) -> Result<Self> {
        Self::new(serde_json::from_value(value.clone()).map_err(|e| e.to_string())?)
    }
    pub fn type_id(&self) -> &str {
        &self.raw.type_id
    }
    pub fn daily(&self) -> bool {
        self.raw.type_id == "futures.daily"
    }
    fn fields(&self) -> &'static [&'static str] {
        if self.daily() {
            &DAILY_FIELDS
        } else {
            &BAR_FIELDS
        }
    }
    fn required(&self) -> Vec<&'static str> {
        if self.daily() {
            DAILY_FIELDS[..7].to_vec()
        } else {
            BAR_FIELDS
                .iter()
                .copied()
                .filter(|f| *f != "trading_day")
                .collect()
        }
    }
    /// The published form of these options (defaults filled in).
    pub fn dump(&self) -> Value {
        let mapping: Map<String, Value> = self.raw.column_mapping.0.iter().cloned().collect();
        json!({
            "identity": self.identity.model(),
            "trading_time": self.raw.trading_time.clone().unwrap_or(Value::Null),
            "timestamp_semantics": self.raw.timestamp_semantics,
            "type_id": self.raw.type_id,
            "frequency": self.raw.frequency,
            "source_id": self.raw.source_id,
            "column_mapping": mapping,
        })
    }
    pub fn frequency(&self) -> &str {
        &self.raw.frequency
    }
    pub fn source_id(&self) -> &str {
        &self.raw.source_id
    }
    pub fn resolve(&self, contract: &str, day: &str) -> Result<String> {
        let day = NaiveDate::parse_from_str(day, "%Y-%m-%d").map_err(|e| e.to_string())?;
        Ok(self.identity.resolve(contract, day)?.id)
    }
    pub fn catalog_inputs(&self) -> Vec<(String, String, String)> {
        self.identity
            .model()
            .catalog
            .inputs
            .iter()
            .map(|input| {
                (
                    input.version_id.clone(),
                    input.checksum.clone(),
                    input.source.clone(),
                )
            })
            .collect()
    }
}

/// Header and records of a CSV text, strictly: quoting errors, blank or
/// repeated headers and records of another width are refused.
pub fn parse(content: &str) -> Result<(Vec<String>, Vec<Vec<String>>)> {
    if content.len() > LIMIT {
        return Err("CSV 超过 2 MB".into());
    }
    let text = content.trim_start_matches('\u{feff}');
    let mut records: Vec<Vec<String>> = Vec::new();
    let mut record: Vec<String> = Vec::new();
    let mut field = String::new();
    let (mut quoted, mut closed, mut started) = (false, false, false);
    let mut chars = text.chars().peekable();
    let format_error = || "CSV 引号或记录格式错误".to_string();
    while let Some(c) = chars.next() {
        if quoted {
            match c {
                '"' if chars.peek() == Some(&'"') => {
                    chars.next();
                    field.push('"');
                }
                '"' => {
                    quoted = false;
                    closed = true;
                }
                _ => field.push(c),
            }
            continue;
        }
        match c {
            '"' if field.is_empty() && !closed => {
                quoted = true;
                started = true;
            }
            ',' => {
                record.push(std::mem::take(&mut field));
                closed = false;
                started = true;
            }
            '\r' if chars.peek() == Some(&'\n') => {}
            '\n' | '\r' => {
                if started || !record.is_empty() || !field.is_empty() {
                    record.push(std::mem::take(&mut field));
                }
                records.push(std::mem::take(&mut record));
                closed = false;
                started = false;
            }
            _ if closed => return Err(format_error()),
            _ => {
                field.push(c);
                started = true;
            }
        }
    }
    if quoted {
        return Err(format_error());
    }
    if started || !record.is_empty() || !field.is_empty() {
        record.push(field);
        records.push(record);
    }
    let mut records = records.into_iter();
    let names = records.next().unwrap_or_default();
    let unique: BTreeSet<&String> = names.iter().collect();
    if names.is_empty() || unique.len() != names.len() || names.iter().any(|n| n.trim().is_empty())
    {
        return Err("CSV 表头不能为空或重复".into());
    }
    // Blank lines are not records.
    let rows: Vec<Vec<String>> = records.filter(|r| !r.is_empty()).collect();
    if rows.is_empty() || rows.iter().any(|r| r.len() != names.len()) {
        return Err("CSV 为空或数据列数与表头不一致".into());
    }
    Ok((names, rows))
}

fn positive_price(text: &str) -> Result<()> {
    let lowered = text.trim().to_ascii_lowercase();
    let unsigned = lowered.trim_start_matches(['+', '-']);
    if matches!(unsigned, "nan" | "snan" | "inf" | "infinity") {
        return Err("当前日线图表要求价格为有限正数".into());
    }
    let canonical = decimal_text(text).ok_or("价格必须为有效数字")?;
    let value: f64 = canonical.parse().map_err(|_| "价格必须为有效数字")?;
    if !value.is_finite() || value <= 0.0 {
        return Err("当前日线图表要求价格为有限正数".into());
    }
    Ok(())
}

fn stamp(text: &str) -> Result<DateTime<FixedOffset>> {
    let normalized = text.replacen(' ', "T", 1);
    DateTime::parse_from_rfc3339(&normalized)
        .or_else(|_| DateTime::parse_from_str(&normalized, "%Y-%m-%dT%H:%M%:z"))
        .map_err(|_| format!("Invalid isoformat string: '{text}'"))
}

fn text<'a>(row: &'a Row, name: &str) -> &'a str {
    row.get(name).and_then(Value::as_str).unwrap_or_default()
}

fn set(row: &mut Row, name: &str, value: Value) {
    match row.0.iter_mut().find(|(key, _)| key == name) {
        Some(entry) => entry.1 = value,
        None => row.0.push((name.into(), value)),
    }
}

/// Records mapped onto the type's fields, checked and sorted by primary key.
pub fn mapped(content: &str, options: &ImportOptions) -> Result<Vec<Row>> {
    let (names, records) = parse(content)?;
    let fields = options.fields();
    let required = options.required();
    let mapping: Vec<(String, String)> = if options.raw.column_mapping.0.is_empty() {
        fields
            .iter()
            .filter(|f| names.iter().any(|n| n == *f))
            .map(|f| ((*f).to_string(), (*f).to_string()))
            .collect()
    } else {
        options
            .raw
            .column_mapping
            .0
            .iter()
            .map(|(key, column)| (key.clone(), column.as_str().unwrap_or_default().to_string()))
            .collect()
    };
    if mapping
        .iter()
        .any(|(key, column)| !fields.contains(&key.as_str()) || !names.iter().any(|n| n == column))
    {
        return Err("字段映射包含不存在的字段或列名".into());
    }
    let columns: BTreeSet<&String> = mapping.iter().map(|(_, c)| c).collect();
    if columns.len() != mapping.len() {
        return Err("同一文件列不能映射到多个标准字段".into());
    }
    let missing: Vec<&str> = required
        .iter()
        .copied()
        .filter(|key| !mapping.iter().any(|(k, _)| k == key))
        .collect();
    if !missing.is_empty() {
        return Err(format!("缺少必填字段映射：{}", missing.join(", ")));
    }
    let position = |column: &str| {
        names
            .iter()
            .position(|n| n == column)
            .expect("mapped column")
    };
    let mut output: Vec<Row> = records
        .iter()
        .map(|record| {
            Row(mapping
                .iter()
                .map(|(key, column)| (key.clone(), json!(record[position(column)].trim())))
                .collect())
        })
        .collect();
    if options.daily() {
        let today = Utc::now().date_naive();
        for row in &mut output {
            let day = iso_date(text(row, "trading_day"))?;
            set(
                row,
                "trading_day",
                json!(day.format("%Y-%m-%d").to_string()),
            );
            if day > today {
                return Err("历史日线不能包含未来日期".into());
            }
            for field in ["open", "high", "low", "close"] {
                positive_price(text(row, field))?;
            }
            let contract = text(row, "contract").to_string();
            if !DAILY_CONTRACT.is_match(&contract) {
                return Err("标准合约格式应为交易所.合约代码".into());
            }
            let (exchange, symbol) = contract.split_once('.').expect("matched contract");
            set(row, "exchange", json!(exchange));
            set(row, "symbol", json!(symbol));
            for field in ["amount", "oi", "settle"] {
                let value = row
                    .get(field)
                    .cloned()
                    .filter(|v| v.as_str().is_some_and(|s| !s.is_empty()));
                set(row, field, value.unwrap_or(Value::Null));
            }
        }
        let contracts: BTreeSet<&str> = output.iter().map(|row| text(row, "contract")).collect();
        if contracts.len() != 1 {
            return Err("日线文件请按单个合约导入，保证数据集范围明确".into());
        }
    }
    if let Some(calendar) = &options.calendar {
        for row in &mut output {
            let contract = text(row, "contract").to_string();
            if options.daily() {
                let day = NaiveDate::parse_from_str(text(row, "trading_day"), "%Y-%m-%d")
                    .map_err(|e| e.to_string())?;
                calendar.daily(&contract, day)?;
                continue;
            }
            let semantics = options
                .raw
                .timestamp_semantics
                .as_deref()
                .unwrap_or("bar_start");
            let seconds = match options.raw.frequency.as_str() {
                "1m" => 60,
                "5m" => 300,
                "15m" => 900,
                "30m" => 1800,
                "1h" => 3600,
                _ => return Err("日内行情频率无效".into()),
            };
            let event = stamp(text(row, "event_time"))?;
            let boundary = if semantics == "bar_end" {
                Boundary::BarEnd
            } else {
                Boundary::Event
            };
            let resolved = calendar.resolve(&contract, event, boundary)?;
            if row.get("trading_day").is_none() {
                set(
                    row,
                    "trading_day",
                    json!(resolved.trading_day.format("%Y-%m-%d").to_string()),
                );
            }
            let day = NaiveDate::parse_from_str(text(row, "trading_day"), "%Y-%m-%d")
                .map_err(|_| format!("Invalid isoformat string: '{}'", text(row, "trading_day")))?;
            let bar = if semantics == "bar_end" {
                BarBoundary::BarEnd
            } else {
                BarBoundary::BarStart
            };
            calendar.validate_bar(&contract, event, day, seconds, bar)?;
        }
    }
    let values: Vec<Value> = output.iter().map(row_value).collect();
    options.identity.validate_rows(&values)?;
    types::validate(options.type_id(), &values)?;
    let key = types::manifest_of(options.type_id())?.primary_key;
    output.sort_by(|a, b| {
        let fields = |row: &Row| {
            key.iter()
                .map(|k| text(row, k).to_string())
                .collect::<Vec<_>>()
        };
        fields(a).cmp(&fields(b))
    });
    Ok(output)
}

fn row_value(row: &Row) -> Value {
    Value::Object(row.0.iter().cloned().collect())
}

/// The standard table of an import and its manifest.
pub fn encode(csv: &str, options: &ImportOptions) -> Result<(Vec<u8>, Value, Vec<Row>)> {
    let mut rows = mapped(csv, options)?;
    if !options.daily() {
        let values: Vec<Value> = rows.iter().map(row_value).collect();
        let (content, mut manifest) = encode_bars(&values)?;
        manifest["frequency"] = json!(options.frequency());
        return Ok((content, manifest, rows));
    }
    let bars: Vec<Value> = rows
        .iter()
        .map(|row| {
            let day = text(row, "trading_day");
            let volume = decimal_text(text(row, "vol")).ok_or("成交量无效")?;
            let whole: f64 = volume.parse().map_err(|_| "成交量无效")?;
            Ok(
                json!({"contract": text(row, "contract"), "event_time": format!("{day}T00:00:00Z"),
                      "available_at": format!("{day}T00:00:00Z"), "trading_day": day,
                      "open": text(row, "open"), "high": text(row, "high"), "low": text(row, "low"),
                      "close": text(row, "close"), "volume": whole.trunc() as i64}),
            )
        })
        .collect::<Result<_>>()?;
    types::validate("futures.bars", &bars)?;
    // Standard storage keeps canonical scalar strings, not chart timestamps.
    for row in &mut rows {
        for (key, value) in &mut row.0 {
            if matches!(
                key.as_str(),
                "open" | "high" | "low" | "close" | "vol" | "amount" | "oi" | "settle"
            ) && let Some(text) = value.as_str()
            {
                *value = json!(decimal_text(text).ok_or("数值无效")?);
            }
        }
    }
    let content = encode_table(&rows)?;
    let manifest = json!({
        "rows": rows.len(),
        "contracts": [text(&rows[0], "contract")],
        "frequency": "1d",
        "demo": text(&rows[0], "exchange") == "SIM",
        "start": text(&rows[0], "trading_day"),
        "end": text(&rows[rows.len() - 1], "trading_day"),
    });
    Ok((content, manifest, rows))
}

/// The catalogue inputs of the identity must be current, intact standard
/// contract versions.
pub fn validate_inputs(
    tx: &Transaction,
    store: &ArtifactStore,
    options: &ImportOptions,
) -> Result<()> {
    let schema = types::manifest_of("futures.contracts")?.schema_version;
    for (version_id, checksum, source) in options.catalog_inputs() {
        let found = preview(tx, store, &version_id, 0, 1).map_err(|error| match error {
            PreviewError::NotFound => "导入身份目录的固定资料版本不存在".to_string(),
            PreviewError::Refused(message) => message,
            PreviewError::Store(error) => error.to_string(),
        })?;
        let manifest = &found.version["manifest"];
        if manifest["source"] != source.as_str()
            || manifest["checksum"] != checksum.as_str()
            || manifest["layer"] != "STANDARD"
            || manifest["state"] != "PUBLISHED"
            || manifest["demo"].as_bool().unwrap_or(false)
            || manifest["type"]["id"] != "futures.contracts"
            || manifest["type"]["schema_version"] != schema
        {
            return Err("目录输入不是匹配的当前标准合约资料版本".into());
        }
    }
    Ok(())
}

/// The mapping preview of an import request; rows keep the mapped order.
#[derive(Debug, serde::Serialize)]
pub struct ImportPreview {
    pub contract_ids: Vec<String>,
    pub columns: Vec<String>,
    pub fields: Vec<&'static str>,
    pub required: Vec<&'static str>,
    pub rows: Vec<Row>,
    pub total: usize,
    pub valid: bool,
    pub errors: Vec<String>,
}

/// Unreadable files are refused; other problems are reported in the preview.
pub fn preview_import(csv: &str, options: &ImportOptions) -> Result<ImportPreview> {
    let (columns, _) = parse(csv)?;
    let fields = options.fields().to_vec();
    let required = options.required();
    let outcome = encode(csv, options).and_then(|(_, _, rows)| {
        let ids = rows
            .iter()
            .map(|row| options.resolve(text(row, "contract"), text(row, "trading_day")))
            .collect::<Result<BTreeSet<_>>>()?;
        Ok((rows, ids))
    });
    Ok(match outcome {
        Ok((rows, ids)) => ImportPreview {
            contract_ids: ids.into_iter().collect(),
            columns,
            fields,
            required,
            total: rows.len(),
            rows: rows.into_iter().take(10).collect(),
            valid: true,
            errors: vec![],
        },
        Err(error) => ImportPreview {
            contract_ids: vec![],
            columns,
            fields,
            required,
            rows: vec![],
            total: 0,
            valid: false,
            errors: vec![error],
        },
    })
}

fn put(store: &ArtifactStore, name: &str, content: &[u8]) -> Result<ArtifactRef> {
    store.put(name, content).map_err(|e| e.to_string())
}

/// Register a RAW and a STANDARD version of one import in the caller's
/// transaction; returns their identifiers.
#[allow(clippy::too_many_arguments)]
pub fn publish_pair(
    tx: &Transaction,
    job_id: &str,
    type_id: &str,
    source: &str,
    scope: &Value,
    raw: (&ArtifactRef, &str),
    standard: (&ArtifactRef, &str),
    rows: usize,
    snapshot_id: &str,
    detail: &Map<String, Value>,
    now: f64,
) -> Result<Vec<String>> {
    let definition = types::manifest_of(type_id)?;
    let mut type_value = serde_json::to_value(&definition).map_err(|e| e.to_string())?;
    if let Some(frequency) = scope["frequency"].as_str().filter(|f| !f.is_empty()) {
        type_value["frequency"] = json!(frequency);
    }
    let mut inputs: Vec<String> = Vec::new();
    let mut ids = Vec::new();
    for (layer, (artifact, format)) in [("RAW", raw), ("STANDARD", standard)] {
        let identity = json!({
            "type_id": type_id, "source": source, "scope": scope, "layer": layer,
            "schema_version": definition.schema_version,
            "frequency": scope.get("frequency").cloned().unwrap_or(json!(definition.frequency)),
        });
        let dataset_id = stable_id(&identity);
        tx.execute(
            "INSERT INTO data_collections (id, type_id, domain, source, layer, identity) \
             VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (id) DO NOTHING",
            vec![
                json!(dataset_id),
                json!(type_id),
                json!(definition.domain),
                json!(source),
                json!(layer),
                json!(identity.to_string()),
            ],
        )
        .map_err(|e| e.to_string())?;
        let version_id = stable_id(&json!({"dataset_id": dataset_id, "job_id": job_id}));
        let standard = layer == "STANDARD";
        let mut manifest = json!({
            "type": type_value, "source": source, "layer": layer, "scope": scope,
            "format": format, "checksum": artifact.sha256, "bytes": artifact.bytes,
            "path": artifact.name, "inputs": inputs, "state": "PUBLISHED",
            "quality": if standard { "VALIDATED" } else { "CAPTURED" },
            "snapshot_id": if standard { json!(snapshot_id) } else { Value::Null },
            "transform": if standard {
                json!({"id": format!("normalize:{source}"), "version": "1",
                       "plugin_digest": detail.get("plugin_digest").cloned().unwrap_or(Value::Null),
                       "output_schema": definition.schema_version})
            } else { Value::Null },
            "version_semantics": "ACQUISITION_SCOPE",
        });
        for (key, value) in detail {
            manifest[key] = value.clone();
        }
        tx.execute(
            "INSERT INTO data_versions (id, dataset_id, job_id, created_at, rows, manifest) \
             VALUES (?, ?, ?, ?, ?, ?)",
            vec![
                json!(version_id),
                json!(dataset_id),
                json!(job_id),
                json!(now),
                json!(rows),
                json!(manifest.to_string()),
            ],
        )
        .map_err(|e| e.to_string())?;
        inputs = vec![version_id.clone()];
        ids.push(version_id);
    }
    Ok(ids)
}

/// Publish a claimed import task: versions, chart snapshot and artifacts, in
/// the caller's transaction, which also holds the task's lease.
pub fn publish(
    tx: &Transaction,
    store: &ArtifactStore,
    job_id: &str,
    payload: &Value,
    created_at: f64,
    now: f64,
) -> Result<Value> {
    if payload.get("options").is_none_or(Value::is_null) {
        return Err("导入任务缺少当前数据规范，请重新提交".into());
    }
    let options = ImportOptions::from_value(&payload["options"])?;
    let csv = payload["csv"].as_str().ok_or("导入任务缺少文件内容")?;
    let source = payload["source"].as_str().ok_or("导入任务缺少来源名称")?;
    let (standard_content, import_manifest, rows) = encode(csv, &options)?;
    validate_inputs(tx, store, &options)?;
    let (chart, mut manifest) = if options.daily() {
        let available = DateTime::from_timestamp_micros((created_at * 1e6).round() as i64)
            .ok_or("任务时间无效")?;
        let values: Vec<Value> = rows.iter().map(row_value).collect();
        daily_chart(&values, &python_iso(available))?
    } else {
        (standard_content.clone(), import_manifest.clone())
    };
    let snapshot_id = uuid::Uuid::new_v4().to_string();
    let published = put(store, &format!("published/{snapshot_id}.parquet"), &chart)?;
    manifest["uri"] = json!(format!("asterion://local/published/{snapshot_id}"));
    manifest["source"] = json!(source);
    manifest["state"] = json!("PUBLISHED");
    manifest["import_checksum"] = json!(hex::encode(Sha256::digest(&standard_content)));
    let raw = put(
        store,
        &format!("imports/{snapshot_id}/source.csv"),
        csv.as_bytes(),
    )?;
    let standard = if options.daily() {
        put(
            store,
            &format!("imports/{snapshot_id}/standard.parquet"),
            &standard_content,
        )?
    } else {
        published.clone()
    };
    let contract_ids = rows
        .iter()
        .map(|row| options.resolve(text(row, "contract"), text(row, "trading_day")))
        .collect::<Result<BTreeSet<_>>>()?;
    let mut scope = json!({
        "contract_ids": contract_ids, "source_id": options.source_id(),
        "contracts": manifest["contracts"], "frequency": options.frequency(),
    });
    if options.daily() {
        let first = manifest["contracts"][0].as_str().unwrap_or_default();
        let (exchange, symbol) = first.split_once('.').ok_or("日线合约无效")?;
        scope["exchange"] = json!(exchange);
        scope["symbol"] = json!(symbol);
    }
    let detail: Map<String, Value> = json!({
        "demo": manifest["demo"],
        "origin": {"method": "file", "name": source, "contract_ids": contract_ids,
                   "source_id": options.source_id()},
        "import_options": options.dump(),
        "first": import_manifest["start"],
        "last": import_manifest["end"],
        "coverage": "IMPORTED_ROWS_ONLY",
        "empty_partitions": [],
    })
    .as_object()
    .cloned()
    .expect("object");
    let count = manifest["rows"].as_u64().unwrap_or_default() as usize;
    publish_pair(
        tx,
        job_id,
        options.type_id(),
        "local_file",
        &scope,
        (&raw, "csv"),
        (&standard, "parquet"),
        count,
        &snapshot_id,
        &detail,
        now,
    )?;
    tx.execute(
        "INSERT INTO snapshots (id, job_id, manifest) VALUES (?, ?, ?)",
        vec![
            json!(snapshot_id),
            json!(job_id),
            json!(manifest.to_string()),
        ],
    )
    .map_err(|e| e.to_string())?;
    Ok(json!({"id": snapshot_id, "job_id": job_id, "manifest": manifest}))
}

/// Python's `datetime.isoformat()` of a UTC time.
fn python_iso(value: DateTime<Utc>) -> String {
    let mut text = value.format("%Y-%m-%dT%H:%M:%S").to_string();
    if value.timestamp_subsec_micros() != 0 {
        text.push_str(&format!(".{:06}", value.timestamp_subsec_micros()));
    }
    text + "+00:00"
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn csv_parsing_is_strict_and_dates_follow_fromisoformat() {
        let (names, rows) = parse("\u{feff}a,b\r\n1,\"x,\"\"y\"\"\"\n\n2,3\n").unwrap();
        assert_eq!(names, ["a", "b"]);
        assert_eq!(
            rows,
            [
                vec!["1".to_string(), "x,\"y\"".into()],
                vec!["2".into(), "3".into()]
            ]
        );
        assert_eq!(parse("a,a\n1,2").unwrap_err(), "CSV 表头不能为空或重复");
        assert_eq!(
            parse("a,b\n1").unwrap_err(),
            "CSV 为空或数据列数与表头不一致"
        );
        assert_eq!(parse("a\n\"1\"x\n").unwrap_err(), "CSV 引号或记录格式错误");
        assert_eq!(parse("a\n\"1\n").unwrap_err(), "CSV 引号或记录格式错误");
        assert_eq!(parse("a\n").unwrap_err(), "CSV 为空或数据列数与表头不一致");
        for (text, day) in [
            ("2026-01-05", "2026-01-05"),
            ("20260105", "2026-01-05"),
            ("2026-W02-1", "2026-01-05"),
            ("2026W021", "2026-01-05"),
        ] {
            assert_eq!(iso_date(text).unwrap().format("%Y-%m-%d").to_string(), day);
        }
        assert_eq!(
            iso_date("2026/01/05").unwrap_err(),
            "Invalid isoformat string: '2026/01/05'"
        );
        assert_eq!(
            positive_price("NaN").unwrap_err(),
            "当前日线图表要求价格为有限正数"
        );
        assert_eq!(
            positive_price("-1").unwrap_err(),
            "当前日线图表要求价格为有限正数"
        );
        assert_eq!(positive_price("abc").unwrap_err(), "价格必须为有效数字");
    }
}

#[derive(Debug)]
pub enum ExecuteError {
    /// The lease is no longer current; nothing is recorded.
    Lease(String),
    /// The import cannot be published; the task fails with this reason.
    Refused(String),
}

/// Execute a claimed import task in the caller's write transaction: verify
/// its lease, publish its versions and complete it.
pub fn execute(
    tx: &Transaction,
    store: &ArtifactStore,
    tasks: &asterion_kernel::tasks::repository::Repository,
    job_id: &str,
    token: &str,
    trace: Value,
    now: f64,
) -> std::result::Result<Value, ExecuteError> {
    use asterion_kernel::tasks::{Action, repository::Request};
    let lease = |trace: Value| {
        tasks
            .run(
                tx.connection(),
                Request::Lease {
                    id: job_id.into(),
                    token: token.into(),
                    now,
                },
                trace,
            )
            .map_err(|error| ExecuteError::Lease(error.to_string()))
    };
    let job = lease(trace.clone())?;
    if job["kind"] != KIND {
        return Err(ExecuteError::Refused("任务不是文件导入".into()));
    }
    let created_at = job["created_at"].as_f64().unwrap_or(now);
    let record = publish(tx, store, job_id, &job["payload"], created_at, now)
        .map_err(ExecuteError::Refused)?;
    let result: Map<String, Value> = json!({"snapshot_id": record["id"]})
        .as_object()
        .cloned()
        .expect("object");
    tasks
        .run(
            tx.connection(),
            Request::Apply {
                id: job_id.into(),
                action: Action::Complete {
                    token: token.into(),
                    result,
                },
                now,
            },
            trace,
        )
        .map_err(|error| ExecuteError::Lease(error.to_string()))?;
    Ok(record)
}
