use crate::{Error, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

pub type Values = BTreeMap<String, String>;
pub const MAX_PROFILES: usize = 256;
pub const MAX_CONNECTORS: usize = 64;
pub const MAX_FIELDS: usize = 128;
pub const MAX_TEXT_BYTES: usize = 8192;
pub const MAX_STATE_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_READ_BYTES: usize = 16 * 1024 * 1024;

pub fn validate_values(values: &Values) -> Result<()> {
    if values.len() > MAX_FIELDS
        || values
            .iter()
            .any(|(k, v)| k.len() > 128 || v.len() > MAX_TEXT_BYTES)
    {
        return Err(Error::invalid("配置超出字段或文本预算"));
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, JsonSchema)]
pub enum Exchange {
    SHFE,
    DCE,
    CZCE,
    CFFEX,
    INE,
    GFEX,
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct Subscription {
    #[schemars(with = "Exchange")]
    pub exchange: String,
    pub symbol: String,
}
impl Subscription {
    pub fn validate(&self) -> Result<()> {
        serde_json::from_value::<Exchange>(serde_json::json!(&self.exchange))
            .map_err(|_| Error::invalid("交易所不受支持"))?;
        let letters = self
            .symbol
            .bytes()
            .take_while(|c| c.is_ascii_alphabetic())
            .count();
        let digits = &self.symbol.as_bytes()[letters..];
        let expected = if self.exchange == "CZCE" { 3 } else { 4 };
        if !(1..=3).contains(&letters)
            || digits.len() != expected
            || !digits.iter().all(u8::is_ascii_digit)
        {
            return Err(Error::invalid("请输入实际月份合约代码"));
        }
        let month = (digits[expected - 2] - b'0') * 10 + digits[expected - 1] - b'0';
        if !(1..=12).contains(&month) {
            return Err(Error::invalid(
                "请输入月份合约代码，不支持主力、连续或指数代码",
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ConfigField {
    pub key: String,
    pub label: String,
    #[serde(default)]
    pub secret: bool,
    #[serde(default = "yes")]
    pub required: bool,
    #[serde(default)]
    pub identity: bool,
    #[serde(default)]
    pub default: String,
}

fn yes() -> bool {
    true
}
fn version() -> u32 {
    1
}
fn initial_detail() -> String {
    "尚未连接".into()
}
fn version_schema(schema: &mut schemars::Schema) {
    schema.insert("const".into(), serde_json::json!(1));
}
fn complete_schema(schema: &mut schemars::Schema) {
    schema.insert("const".into(), serde_json::json!(true));
}

impl ConfigField {
    pub fn validate(&self) -> Result<()> {
        if self.key.len() > 128
            || self.label.len() > MAX_TEXT_BYTES
            || self.default.len() > MAX_TEXT_BYTES
            || !self.key.starts_with(|c: char| c.is_ascii_lowercase())
            || !self
                .key
                .bytes()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_')
            || (self.secret && (!self.default.is_empty() || self.identity))
        {
            return Err(Error::invalid("接入字段契约无效"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ConnectorDescriptor {
    pub id: String,
    pub owner: String,
    #[serde(default = "version")]
    #[schemars(transform = version_schema)]
    pub version: u32,
    pub title: String,
    #[serde(default)]
    pub instructions: String,
    pub capabilities: Vec<Feature>,
    pub fields: Vec<ConfigField>,
}
impl ConnectorDescriptor {
    pub fn validate(&self) -> Result<()> {
        if self.id.is_empty()
            || self.owner.is_empty()
            || self.version != 1
            || self.id.len() > 128
            || self.owner.len() > 128
            || self.title.len() > MAX_TEXT_BYTES
            || self.instructions.len() > MAX_TEXT_BYTES
            || self.fields.len() > MAX_FIELDS
        {
            return Err(Error::invalid("接入贡献身份或版本无效"));
        }
        let mut keys = BTreeSet::new();
        for field in &self.fields {
            field.validate()?;
            if !keys.insert(&field.key) {
                return Err(Error::invalid("接入字段契约无效"));
            }
        }
        if self.capabilities.iter().collect::<BTreeSet<_>>().len() != self.capabilities.len() {
            return Err(Error::invalid("重复接入能力"));
        }
        Ok(())
    }
    pub fn supports(&self, feature: Feature) -> bool {
        self.capabilities.contains(&feature)
    }
}

#[derive(
    Clone, Copy, Debug, Deserialize, Serialize, JsonSchema, PartialEq, Eq, PartialOrd, Ord,
)]
#[serde(rename_all = "snake_case")]
pub enum Feature {
    MarketQuotes,
    InstrumentCatalog,
    AccountSnapshot,
    Positions,
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ConnectionProfile {
    pub connection_id: String,
    pub connector_id: String,
    pub name: String,
    pub config_revision: u64,
    pub config: Values,
}
impl ConnectionProfile {
    pub fn validate(&self) -> Result<()> {
        validate_values(&self.config)?;
        if !identifier(&self.connection_id)
            || self.config_revision == 0
            || !(1..=60).contains(&self.name.chars().count())
            || self.name.trim().is_empty()
            || self.connector_id.is_empty()
        {
            return Err(Error::invalid("连接配置身份无效"));
        }
        Ok(())
    }
}
pub fn identifier(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Saved {
    #[serde(flatten)]
    pub profile: ConnectionProfile,
    pub encrypted: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SecretChange {
    pub action: SecretAction,
    pub value: Option<String>,
}
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SecretAction {
    Keep,
    Replace,
    Clear,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Save {
    pub connection_id: Option<String>,
    pub expected_revision: Option<u64>,
    pub connector_id: String,
    pub name: String,
    pub config: Values,
    pub secrets: BTreeMap<String, SecretChange>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Stored {
    pub version: u32,
    pub connections: Vec<Saved>,
    pub selected_id: Option<String>,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, JsonSchema, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Channel {
    Market,
    Account,
}
#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, JsonSchema, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ConnectionStatus {
    #[default]
    Disconnected,
    Connecting,
    Authenticating,
    Ready,
    Reconnecting,
    Error,
}
#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ChannelState {
    #[serde(default)]
    pub state: ConnectionStatus,
    #[serde(default)]
    pub generation: u64,
    #[serde(default = "initial_detail")]
    pub detail: String,
}
impl ChannelState {
    pub fn validate(&self) -> Result<()> {
        if self.detail.len() > MAX_TEXT_BYTES {
            Err(Error::invalid("通道说明超出文本预算"))
        } else {
            Ok(())
        }
    }
}
impl Default for ChannelState {
    fn default() -> Self {
        Self {
            state: ConnectionStatus::Disconnected,
            generation: 0,
            detail: "尚未连接".into(),
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ReadKind {
    Instruments,
    Account,
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ReadRequest {
    pub connection_id: String,
    pub generation: u64,
    pub request_id: String,
    pub started_at: f64,
}
impl ReadRequest {
    pub fn validate(&self) -> Result<()> {
        if !identifier(&self.connection_id)
            || !identifier(&self.request_id)
            || !self.started_at.is_finite()
            || self.started_at < 0.
        {
            return Err(Error::invalid("查询身份或开始时间无效"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ReadBatch {
    pub connection_id: String,
    pub generation: u64,
    pub request_id: String,
    pub started_at: f64,
    pub observed_at: f64,
    #[schemars(transform = complete_schema)]
    pub complete: bool,
}
impl ReadBatch {
    pub fn validate(&self) -> Result<()> {
        ReadRequest {
            connection_id: self.connection_id.clone(),
            generation: self.generation,
            request_id: self.request_id.clone(),
            started_at: self.started_at,
        }
        .validate()?;
        if !self.complete
            || !self.observed_at.is_finite()
            || self.observed_at < self.started_at
            || self.observed_at > crate::now() + 5.
        {
            return Err(Error::invalid("查询响应时间或完整性无效"));
        }
        Ok(())
    }
}
