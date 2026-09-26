//! The data source port. Built-in adapters implement `Provider`: they own
//! transport and vendor field interpretation; plans are checked here, and
//! normalized rows are validated by the data types before anything is stored.
use chrono::{DateTime, FixedOffset, NaiveDate};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

pub type Result<T> = std::result::Result<T, String>;

/// A bounded source time window (intraday requests).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceWindow {
    pub start: DateTime<FixedOffset>,
    pub end: DateTime<FixedOffset>,
}

/// One collection request of a data type from a source.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SyncRequest {
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
    pub window: Option<SourceWindow>,
    #[serde(default)]
    pub start: Option<NaiveDate>,
    #[serde(default)]
    pub end: Option<NaiveDate>,
}

impl SyncRequest {
    /// Request constraints; `today` is the current exchange-local date.
    pub fn validate(&self, today: NaiveDate) -> Result<()> {
        let within =
            |text: &str, min: usize, max: usize| (min..=max).contains(&text.chars().count());
        let pattern =
            |pattern: &str, text: &str| regex::Regex::new(pattern).expect("pattern").is_match(text);
        let window = self.window.as_ref().is_none_or(|window| {
            window.start <= window.end && (window.end - window.start).num_seconds() <= 10 * 86400
        });
        let dates = match (self.start, self.end) {
            (None, None) => true,
            (Some(start), Some(end)) => {
                start <= end && (end - start).num_days() <= 3660 && end <= today
            }
            _ => false,
        };
        let valid = within(&self.command_id, 1, 100)
            && pattern(r"^[a-z][a-z0-9_]{0,40}$", &self.provider)
            && self
                .connection_id
                .as_deref()
                .is_none_or(|id| pattern(r"^c_[0-9a-f]{32}$", id))
            && within(&self.dataset, 1, 50)
            && within(&self.exchange, 1, 10)
            && within(&self.symbol, 0, 30)
            && self
                .frequency
                .as_deref()
                .is_none_or(|frequency| crate::minute::FREQUENCIES.contains(&frequency))
            && window
            && dates;
        if valid {
            Ok(())
        } else {
            Err("同步请求不符合当前契约".into())
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Capability {
    #[serde(default)]
    pub frequencies: Vec<String>,
    pub id: String,
    pub label: String,
    pub type_id: String,
    pub exchanges: Vec<String>,
    #[serde(default)]
    pub date_range: bool,
    #[serde(default)]
    pub symbol_required: bool,
    pub description: String,
    #[serde(default)]
    pub defaults: Map<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConfigurationField {
    pub id: String,
    pub label: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub secret: bool,
    pub required: bool,
    pub default: Value,
    pub description: String,
    pub placeholder: String,
    pub min_length: Option<u64>,
    pub max_length: Option<u64>,
    pub minimum: Option<i64>,
    pub maximum: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConfigurationSpec {
    pub schema_version: u32,
    pub fields: Vec<ConfigurationField>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProviderManifest {
    pub id: String,
    pub name: String,
    pub version: String,
    pub api_version: u32,
    pub capabilities: Vec<Capability>,
    pub configuration: ConfigurationSpec,
    pub description: String,
    pub demo: bool,
}

/// One bounded request of a vendor interface.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Partition {
    pub api: String,
    pub params: std::collections::BTreeMap<String, String>,
    pub fields: Vec<String>,
    pub limit: u64,
    #[serde(default)]
    pub start: Option<String>,
    #[serde(default)]
    pub end: Option<String>,
}

impl Partition {
    pub fn validate(&self) -> Result<()> {
        if !(1..=100_001).contains(&self.limit) {
            return Err("Invalid partition limit".into());
        }
        match (&self.start, &self.end) {
            (None, None) => {}
            (Some(start), Some(end)) => {
                let day = |text: &str| NaiveDate::parse_from_str(text, "%Y-%m-%d");
                match (day(start), day(end)) {
                    (Ok(start), Ok(end)) if start <= end => {}
                    _ => return Err("Invalid partition window".into()),
                }
            }
            _ => return Err("Partition dates must be paired".into()),
        }
        let unique: std::collections::BTreeSet<&String> = self.fields.iter().collect();
        if self.fields.is_empty() || unique.len() != self.fields.len() {
            return Err("Partition fields must be nonempty and unique".into());
        }
        Ok(())
    }
}

/// A built-in data source.
pub trait Provider: Send + Sync {
    fn manifest(&self) -> ProviderManifest;
    /// The partitions of a request; checked with `checked_plan` by callers.
    fn plan(&self, request: &SyncRequest) -> Result<Vec<Partition>>;
    /// Verify a configuration against the source; returns a user message.
    fn probe(&self, configuration: &Map<String, Value>) -> Result<String>;
    /// Records of one partition, keyed by the vendor's field names.
    fn fetch(
        &self,
        partition: &Partition,
        configuration: &Map<String, Value>,
    ) -> Result<Vec<Value>>;
    /// Vendor records mapped onto the requested data type's fields.
    fn normalize(&self, request: &SyncRequest, rows: &[Value]) -> Result<Vec<crate::table::Row>>;
}

/// The same admission rules apply to every provider's plan: bounded, and for
/// dated requests, contiguous daily partitions covering exactly the request.
pub fn checked_plan(request: &SyncRequest, parts: &[Partition]) -> Result<()> {
    if !(1..=3661).contains(&parts.len()) {
        return Err("采集计划为空或超出限制".into());
    }
    for part in parts {
        part.validate()?;
    }
    let day = |text: &Option<String>| {
        text.as_deref()
            .and_then(|t| NaiveDate::parse_from_str(t, "%Y-%m-%d").ok())
    };
    match (request.start, request.end) {
        (Some(start), Some(end)) => {
            let mut cursor = start;
            for part in parts {
                let (Some(from), Some(to)) = (day(&part.start), day(&part.end)) else {
                    return Err("采集分段日期不连续或不符合请求".into());
                };
                if from != cursor {
                    return Err("采集分段日期不连续或不符合请求".into());
                }
                if to < cursor || to > end {
                    return Err("采集分段日期超出请求".into());
                }
                cursor = to.succ_opt().ok_or("采集分段日期超出请求")?;
            }
            if Some(cursor) != end.succ_opt() {
                return Err("采集计划未覆盖完整请求范围".into());
            }
        }
        _ => {
            if parts.iter().any(|p| p.start.is_some() || p.end.is_some()) {
                return Err("快照请求不接受日期分段".into());
            }
        }
    }
    Ok(())
}
