//! Immutable role facts, knowledge cutoffs and effective openings. Ranking,
//! provider I/O and source verification belong to the application plugins.
use asterion_instrument_catalog::{
    Catalog, Contract, ReferenceCatalog, Validate as CatalogValidate,
};
use asterion_trading_calendar::{
    Boundary, Calendar, Phase, Span, TimeVersion, Validate as CalendarValidate,
};
use chrono::{DateTime, Datelike, FixedOffset, NaiveDate, Timelike};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

pub type Result<T> = std::result::Result<T, String>;
pub trait Validate {
    fn validate(&self) -> Result<()>;
}
fn require(ok: bool, message: &str) -> Result<()> {
    if ok { Ok(()) } else { Err(message.into()) }
}
fn length(value: &str, min: usize, max: usize, field: &str) -> Result<()> {
    require(
        (min..=max).contains(&value.chars().count()),
        &format!("{field}: invalid length"),
    )
}
fn date(value: NaiveDate) -> Result<()> {
    require(
        (1..=9999).contains(&value.year()),
        "date year must be in 1..9999",
    )
}
fn fingerprint(value: &str) -> Result<()> {
    require(
        value.len() == 64
            && value
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)),
        "invalid SHA-256 fingerprint",
    )
}
fn finite_stamp(value: DateTime<FixedOffset>) -> Result<()> {
    date(value.date_naive())?;
    require(
        value.nanosecond() < 1_000_000_000 && value.nanosecond().is_multiple_of(1000),
        "expected microsecond-precision timestamp without leap seconds",
    )
}
fn parsed<T: DeserializeOwned + Validate>(value: Value) -> Result<T> {
    let model: T = serde_json::from_value(value).map_err(|e| e.to_string())?;
    model.validate()?;
    Ok(model)
}
fn output(value: impl Serialize) -> Result<Value> {
    serde_json::to_value(value).map_err(|e| e.to_string())
}
fn trimmed<'de, D: serde::Deserializer<'de>>(d: D) -> std::result::Result<String, D::Error> {
    Ok(String::deserialize(d)?.trim().into())
}
fn text(value: &str, min: usize, max: usize, field: &str) -> Result<()> {
    length(value, min, max, field)?;
    require(value == value.trim(), &format!("{field} must be trimmed"))
}
mod stamp {
    use super::*;
    pub fn serialize<S: serde::Serializer>(
        value: &DateTime<FixedOffset>,
        s: S,
    ) -> std::result::Result<S::Ok, S::Error> {
        let mut result = value.format("%Y-%m-%dT%H:%M:%S").to_string();
        if value.nanosecond() != 0 {
            result.push_str(&format!(".{:06}", value.nanosecond() / 1000));
        }
        let offset = value.offset().local_minus_utc();
        if offset == 0 {
            result.push('Z');
        } else {
            let minutes = offset.unsigned_abs() / 60;
            result.push_str(&format!(
                "{}{:02}:{:02}",
                if offset < 0 { '-' } else { '+' },
                minutes / 60,
                minutes % 60
            ));
        }
        s.serialize_str(&result)
    }
    pub fn parse(value: &str) -> Result<DateTime<FixedOffset>> {
        let value = DateTime::parse_from_rfc3339(value).map_err(|e| e.to_string())?;
        let value = value
            .with_nanosecond(value.nanosecond() / 1000 * 1000)
            .ok_or("invalid timestamp")?;
        finite_stamp(value)?;
        Ok(value)
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(
        d: D,
    ) -> std::result::Result<DateTime<FixedOffset>, D::Error> {
        parse(&String::deserialize(d)?).map_err(serde::de::Error::custom)
    }
}
mod optional_stamp {
    use super::*;
    pub fn serialize<S: serde::Serializer>(
        value: &Option<DateTime<FixedOffset>>,
        s: S,
    ) -> std::result::Result<S::Ok, S::Error> {
        match value {
            Some(value) => stamp::serialize(value, s),
            None => s.serialize_none(),
        }
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(
        d: D,
    ) -> std::result::Result<Option<DateTime<FixedOffset>>, D::Error> {
        Option::<String>::deserialize(d)?
            .map(|value| stamp::parse(&value))
            .transpose()
            .map_err(serde::de::Error::custom)
    }
}

#[derive(
    Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize, JsonSchema,
)]
#[serde(rename_all = "snake_case")]
pub enum Role {
    Main,
    Secondary,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum RoleMode {
    AsKnown,
    Retrospective,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum RoleOrigin {
    ProviderReport,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum AvailabilityBasis {
    LocalObservation,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleReport {
    pub trading_day: NaiveDate,
    pub role: Role,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1))]
    pub contract_id: String,
    #[serde(with = "optional_stamp")]
    #[schemars(with = "Option<DateTime<FixedOffset>>")]
    pub available_at: Option<DateTime<FixedOffset>>,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 2000))]
    pub evidence: String,
}
impl Validate for RoleReport {
    fn validate(&self) -> Result<()> {
        date(self.trading_day)?;
        text(&self.contract_id, 1, usize::MAX, "contract_id")?;
        text(&self.evidence, 1, 2000, "evidence")?;
        if let Some(value) = self.available_at {
            finite_stamp(value)?;
        }
        Ok(())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleSpec {
    #[schemars(range(min = 1, max = 1))]
    pub schema_version: u8,
    pub origin: RoleOrigin,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 100))]
    pub source: String,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 100))]
    pub source_version: String,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub source_checksum: String,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub observed_at: DateTime<FixedOffset>,
    #[serde(deserialize_with = "trimmed")]
    pub product_id: String,
    pub catalog: ReferenceCatalog,
    pub trading_time: TimeVersion,
    #[schemars(length(min = 1, max = 10000))]
    pub reports: Vec<RoleReport>,
}
impl Validate for RoleSpec {
    fn validate(&self) -> Result<()> {
        RoleRegistry::new(self.clone()).map(|_| ())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleVersion {
    #[serde(deserialize_with = "trimmed")]
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub id: String,
    pub spec: RoleSpec,
}
impl Validate for RoleVersion {
    fn validate(&self) -> Result<()> {
        fingerprint(&self.id)?;
        require(
            self.id == RoleRegistry::new(self.spec.clone())?.id,
            "角色版本指纹不一致",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleQuery {
    #[serde(deserialize_with = "trimmed")]
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub version_id: String,
    pub role: Role,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub timestamp: DateTime<FixedOffset>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub information_at: DateTime<FixedOffset>,
    pub mode: RoleMode,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(max = 2000))]
    pub explanation: String,
}
impl Validate for RoleQuery {
    fn validate(&self) -> Result<()> {
        fingerprint(&self.version_id)?;
        finite_stamp(self.timestamp)?;
        finite_stamp(self.information_at)?;
        text(&self.explanation, 0, 2000, "explanation")?;
        require(
            self.mode != RoleMode::Retrospective || !self.explanation.is_empty(),
            "回溯查看必须说明历史可知性未经确认",
        )?;
        require(
            self.mode != RoleMode::AsKnown || self.information_at <= self.timestamp,
            "当时可知查询不能使用未来信息截止时间",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleResolution {
    #[serde(deserialize_with = "trimmed")]
    pub version_id: String,
    pub contract: Contract,
    pub report: RoleReport,
    pub session: Span,
    pub mode: RoleMode,
    #[serde(deserialize_with = "trimmed")]
    pub explanation: String,
    #[schemars(extend("const"=false))]
    pub execution_authorized: bool,
}
impl Validate for RoleResolution {
    fn validate(&self) -> Result<()> {
        self.contract.validate()?;
        self.report.validate()?;
        self.session.validate()?;
        require(!self.execution_authorized, "角色解析不授权交易执行")
    }
}

/// Cached provider report facts. Calendar and lookup indexes are built once.
#[derive(Debug, Clone)]
pub struct RoleRegistry {
    spec: RoleSpec,
    id: String,
    calendar: Calendar,
    contracts: BTreeMap<String, usize>,
    reports: BTreeMap<(NaiveDate, Role), usize>,
}
impl RoleRegistry {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(mut spec: RoleSpec) -> Result<Self> {
        spec.source = spec.source.trim().into();
        spec.source_version = spec.source_version.trim().into();
        spec.source_checksum = spec.source_checksum.trim().into();
        spec.product_id = spec.product_id.trim().into();
        for row in &mut spec.reports {
            row.contract_id = row.contract_id.trim().into();
            row.evidence = row.evidence.trim().into();
        }
        require(spec.schema_version == 1, "unsupported role schema_version")?;
        text(&spec.source, 1, 100, "source")?;
        text(&spec.source_version, 1, 100, "source_version")?;
        fingerprint(&spec.source_checksum)?;
        finite_stamp(spec.observed_at)?;
        Catalog::new(spec.catalog.clone())?;
        spec.trading_time.validate()?;
        let calendar = Calendar::new(spec.trading_time.spec.clone())?;
        require(
            spec.product_id
                == format!(
                    "{}.{}",
                    calendar.spec().exchange.as_str(),
                    calendar.spec().product
                ),
            "角色品种与交易时间不一致",
        )?;
        require(
            (1..=10000).contains(&spec.reports.len()),
            "reports: invalid length",
        )?;
        let contracts: BTreeMap<String, usize> = spec
            .catalog
            .contracts
            .iter()
            .enumerate()
            .map(|(i, c)| (c.id.clone(), i))
            .collect();
        let days: BTreeSet<NaiveDate> = calendar.spans().iter().map(|s| s.trading_day).collect();
        let mut reports = BTreeMap::new();
        let mut assigned = BTreeSet::new();
        for (index, row) in spec.reports.iter().enumerate() {
            row.validate()?;
            require(
                reports.insert((row.trading_day, row.role), index).is_none(),
                "同一交易日角色重复或冲突",
            )?;
            let actual = contracts
                .get(&row.contract_id)
                .map(|i| &spec.catalog.contracts[*i])
                .filter(|c| c.product_id == spec.product_id)
                .ok_or("角色必须指向固定目录中同品种的实际合约")?;
            require(
                actual.listed_on <= row.trading_day && row.trading_day <= actual.last_trade_on,
                "角色超出实际合约生命周期",
            )?;
            require(
                days.contains(&row.trading_day),
                "角色交易日休市或缺少时间覆盖",
            )?;
            require(
                assigned.insert((row.trading_day, &actual.id)),
                "同一交易日主力与次主力不能指向同一合约",
            )?;
            require(
                row.available_at
                    .is_none_or(|known| known <= spec.observed_at),
                "来源可知时刻不能晚于本次观测时刻",
            )?;
        }
        require(
            spec.reports
                .windows(2)
                .all(|r| (r[0].trading_day, r[0].role) < (r[1].trading_day, r[1].role)),
            "角色记录必须按交易日和角色排序",
        )?;
        let id = asterion_foundation::digest(&output(&spec)?)?;
        Ok(Self {
            spec,
            id,
            calendar,
            contracts,
            reports,
        })
    }
    pub fn spec(&self) -> &RoleSpec {
        &self.spec
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn resolve(&self, query: &RoleQuery) -> Result<RoleResolution> {
        query.validate()?;
        require(query.version_id == self.id, "角色查询版本不一致")?;
        let session = self
            .calendar
            .span_at(query.timestamp, Boundary::Event)
            .map_err(|_| "查询时刻不在固定交易时段中")?;
        let index = self
            .reports
            .get(&(session.trading_day, query.role))
            .ok_or("角色映射缺口；不能沿用上一交易日")?;
        let row = &self.spec.reports[*index];
        let actual = &self.spec.catalog.contracts[self.contracts[&row.contract_id]];
        match query.mode {
            RoleMode::AsKnown => require(
                row.available_at
                    .is_some_and(|known| known <= query.information_at),
                "角色历史可知时间未知或晚于信息截止时间",
            )?,
            RoleMode::Retrospective => require(
                self.spec.observed_at <= query.information_at,
                "映射尚未在信息截止时间前观测到",
            )?,
        }
        let product = self
            .spec
            .catalog
            .products
            .iter()
            .find(|p| p.id == actual.product_id)
            .ok_or("角色合约品种资料缺失")?;
        require(
            actual
                .provenance
                .available_at
                .max(product.provenance.available_at)
                <= query.information_at,
            "合约身份资料晚于信息截止时间",
        )?;
        Ok(RoleResolution {
            version_id: self.id.clone(),
            contract: actual.clone(),
            report: row.clone(),
            session: session.clone(),
            mode: query.mode,
            explanation: query.explanation.clone(),
            execution_authorized: false,
        })
    }
}
pub fn role_id(spec: &RoleSpec) -> Result<String> {
    Ok(RoleRegistry::new(spec.clone())?.id)
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoleCalendar {
    #[serde(deserialize_with = "trimmed")]
    pub product_id: String,
    pub trading_time: TimeVersion,
}
impl Validate for RoleCalendar {
    fn validate(&self) -> Result<()> {
        OpeningCalendar::new(self.clone()).map(|_| ())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct NextOpening {
    #[serde(deserialize_with = "trimmed")]
    pub product_id: String,
    pub trading_time: TimeVersion,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub observation_end: DateTime<FixedOffset>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub available_at: DateTime<FixedOffset>,
}
impl Validate for NextOpening {
    fn validate(&self) -> Result<()> {
        self.trading_time.validate()?;
        finite_stamp(self.observation_end)?;
        finite_stamp(self.available_at)
    }
}
#[derive(Debug, Clone)]
pub struct OpeningCalendar {
    model: RoleCalendar,
    calendar: Calendar,
    openings: Vec<usize>,
}
impl OpeningCalendar {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(mut model: RoleCalendar) -> Result<Self> {
        model.product_id = model.product_id.trim().into();
        model.trading_time.validate()?;
        let calendar = Calendar::new(model.trading_time.spec.clone())?;
        require(
            model.product_id
                == format!(
                    "{}.{}",
                    calendar.spec().exchange.as_str(),
                    calendar.spec().product
                ),
            "观测品种与交易时间不一致",
        )?;
        let mut days = BTreeSet::new();
        let openings = calendar
            .spans()
            .iter()
            .enumerate()
            .filter_map(|(index, s)| {
                if s.phase == Phase::Continuous && days.insert(s.trading_day) {
                    Some(index)
                } else {
                    None
                }
            })
            .collect();
        Ok(Self {
            model,
            calendar,
            openings,
        })
    }
    pub fn model(&self) -> &RoleCalendar {
        &self.model
    }
    pub fn next_opening(
        &self,
        observation_end: DateTime<FixedOffset>,
        available_at: DateTime<FixedOffset>,
    ) -> Result<&Span> {
        finite_stamp(observation_end)?;
        finite_stamp(available_at)?;
        require(
            available_at >= observation_end,
            "完整观测窗口结束前不能宣称结果可知",
        )?;
        let spans = self.calendar.spans();
        require(
            spans
                .first()
                .is_some_and(|first| first.start <= observation_end)
                && spans.last().is_some_and(|last| observation_end <= last.end),
            "固定时段未覆盖观测窗口结束时刻",
        )?;
        self.openings
            .iter()
            .map(|i| &spans[*i])
            .find(|span| span.start > observation_end && span.start >= available_at)
            .ok_or_else(|| "固定日历和时段未覆盖下一生效开盘；不能推算或顺延".into())
    }
}
pub fn next_opening(body: &NextOpening) -> Result<Span> {
    body.validate()?;
    Ok(OpeningCalendar::new(RoleCalendar {
        product_id: body.product_id.clone(),
        trading_time: body.trading_time.clone(),
    })?
    .next_opening(body.observation_end, body.available_at)?
    .clone())
}

/// Algorithm-neutral projection of a published computation. It contains no
/// ranking policy, candidates, scores or executable algorithm artifact.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ComputedAssignment {
    pub effective_day: NaiveDate,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub effective_start: DateTime<FixedOffset>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub available_at: DateTime<FixedOffset>,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1))]
    pub main: String,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1))]
    pub secondary: String,
}
impl Validate for ComputedAssignment {
    fn validate(&self) -> Result<()> {
        date(self.effective_day)?;
        finite_stamp(self.effective_start)?;
        finite_stamp(self.available_at)?;
        text(&self.main, 1, usize::MAX, "main")?;
        text(&self.secondary, 1, usize::MAX, "secondary")?;
        require(
            self.main != self.secondary,
            "同一交易日主力与次主力不能指向同一合约",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ComputedRoleIndex {
    #[serde(deserialize_with = "trimmed")]
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub version_id: String,
    #[serde(deserialize_with = "trimmed")]
    pub product_id: String,
    pub catalog: ReferenceCatalog,
    pub trading_time: TimeVersion,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub published_at: DateTime<FixedOffset>,
    #[schemars(length(min = 1, max = 1000))]
    pub records: Vec<ComputedAssignment>,
}
impl Validate for ComputedRoleIndex {
    fn validate(&self) -> Result<()> {
        ComputedRegistry::new(self.clone()).map(|_| ())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ComputedRoleResolution {
    pub version_id: String,
    pub contract: Contract,
    pub record_index: usize,
    pub session: Span,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub available_at: DateTime<FixedOffset>,
    pub availability_basis: AvailabilityBasis,
    #[schemars(extend("const"=false))]
    pub historical_publication_attested: bool,
    #[schemars(extend("const"=false))]
    pub execution_authorized: bool,
}
impl Validate for ComputedRoleResolution {
    fn validate(&self) -> Result<()> {
        self.contract.validate()?;
        self.session.validate()?;
        finite_stamp(self.available_at)?;
        require(
            !self.historical_publication_attested && !self.execution_authorized,
            "计算角色不证明历史公布或交易执行权限",
        )
    }
}
#[derive(Debug, Clone)]
pub struct ComputedRegistry {
    model: ComputedRoleIndex,
    calendar: Calendar,
    records: BTreeMap<NaiveDate, usize>,
    contracts: BTreeMap<String, usize>,
}
impl ComputedRegistry {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(mut model: ComputedRoleIndex) -> Result<Self> {
        model.version_id = model.version_id.trim().into();
        model.product_id = model.product_id.trim().into();
        for record in &mut model.records {
            record.main = record.main.trim().into();
            record.secondary = record.secondary.trim().into();
        }
        fingerprint(&model.version_id)?;
        finite_stamp(model.published_at)?;
        Catalog::new(model.catalog.clone())?;
        let opening = OpeningCalendar::new(RoleCalendar {
            product_id: model.product_id.clone(),
            trading_time: model.trading_time.clone(),
        })?;
        let calendar = opening.calendar;
        let first: BTreeMap<NaiveDate, DateTime<FixedOffset>> = opening
            .openings
            .iter()
            .map(|i| (calendar.spans()[*i].trading_day, calendar.spans()[*i].start))
            .collect();
        require(
            (1..=1000).contains(&model.records.len()),
            "records: invalid length",
        )?;
        let contracts: BTreeMap<String, usize> = model
            .catalog
            .contracts
            .iter()
            .enumerate()
            .map(|(i, c)| (c.id.clone(), i))
            .collect();
        let mut records = BTreeMap::new();
        for (index, record) in model.records.iter().enumerate() {
            record.validate()?;
            require(
                records.insert(record.effective_day, index).is_none(),
                "计算角色生效交易日重复",
            )?;
            require(
                first.get(&record.effective_day) == Some(&record.effective_start),
                "计算角色必须在固定交易日的首个连续交易开盘生效",
            )?;
            require(
                record.available_at <= model.published_at,
                "不能在计算依据可知之前发布角色",
            )?;
            for id in [&record.main, &record.secondary] {
                let actual = contracts
                    .get(id)
                    .map(|i| &model.catalog.contracts[*i])
                    .filter(|c| c.product_id == model.product_id)
                    .ok_or("计算角色必须指向固定目录中同品种的实际合约")?;
                require(
                    actual.listed_on <= record.effective_day
                        && record.effective_day <= actual.last_trade_on,
                    "角色超出实际合约生命周期",
                )?;
            }
        }
        require(
            model
                .records
                .windows(2)
                .all(|p| p[0].effective_day < p[1].effective_day),
            "计算角色记录必须按生效交易日排序",
        )?;
        Ok(Self {
            model,
            calendar,
            records,
            contracts,
        })
    }
    pub fn model(&self) -> &ComputedRoleIndex {
        &self.model
    }
    pub fn resolve(&self, query: &RoleQuery) -> Result<ComputedRoleResolution> {
        query.validate()?;
        require(
            query.version_id == self.model.version_id && query.mode == RoleMode::AsKnown,
            "计算角色要求精确版本与本机当时可知查询，不提供假定历史回填",
        )?;
        let session = self
            .calendar
            .span_at(query.timestamp, Boundary::Event)
            .map_err(|_| "查询时刻不在固定交易时段中")?;
        let index = *self
            .records
            .get(&session.trading_day)
            .ok_or("计算角色尚未生效或存在缺口，不能沿用前值")?;
        let decision = &self.model.records[index];
        require(
            query.timestamp >= decision.effective_start,
            "计算角色尚未生效或存在缺口，不能沿用前值",
        )?;
        require(
            self.model.published_at <= decision.effective_start,
            "角色发布晚于计划生效开盘，不能回填或盘中启用该日角色",
        )?;
        let available_at = decision.available_at.max(self.model.published_at);
        require(
            available_at <= query.information_at,
            "计算依据晚于信息截止时间",
        )?;
        let identifier = match query.role {
            Role::Main => &decision.main,
            Role::Secondary => &decision.secondary,
        };
        let actual = &self.model.catalog.contracts[self.contracts[identifier]];
        let product = self
            .model
            .catalog
            .products
            .iter()
            .find(|p| p.id == actual.product_id)
            .ok_or("角色合约品种资料缺失")?;
        require(
            actual
                .provenance
                .available_at
                .max(product.provenance.available_at)
                <= query.information_at,
            "合约身份资料晚于信息截止时间",
        )?;
        Ok(ComputedRoleResolution {
            version_id: self.model.version_id.clone(),
            contract: actual.clone(),
            record_index: index,
            session: session.clone(),
            available_at,
            availability_basis: AvailabilityBasis::LocalObservation,
            historical_publication_attested: false,
            execution_authorized: false,
        })
    }
}

pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[serde(untagged)]
    #[allow(dead_code)]
    enum Models {
        RoleReport(RoleReport),
        RoleSpec(RoleSpec),
        RoleVersion(RoleVersion),
        RoleQuery(RoleQuery),
        RoleResolution(RoleResolution),
        RoleCalendar(RoleCalendar),
        NextOpening(NextOpening),
        ComputedAssignment(ComputedAssignment),
        ComputedRoleIndex(ComputedRoleIndex),
        ComputedRoleResolution(ComputedRoleResolution),
    }
    let mut schema = output(schemars::schema_for!(Models)).expect("JSON schema is serializable");
    for name in [
        "RoleReport",
        "RoleSpec",
        "RoleVersion",
        "RoleQuery",
        "RoleResolution",
        "RoleCalendar",
        "NextOpening",
        "ComputedAssignment",
        "ComputedRoleIndex",
        "ComputedRoleResolution",
    ] {
        schema["$defs"][name]["x-strip-whitespace"] = Value::Bool(true);
    }
    for (model, field) in [
        ("RoleReport", "available_at"),
        ("Contract", "last_delivery_on"),
    ] {
        schema["$defs"][model]["required"]
            .as_array_mut()
            .unwrap()
            .push(Value::String(field.into()));
    }
    schema
}
pub fn invoke(operation: &str, input: Value) -> Result<Value> {
    macro_rules! request{($name:ident{$($field:ident:$ty:ty),* $(,)?})=>{{#[derive(Deserialize)]#[serde(deny_unknown_fields)]struct $name{$($field:$ty),*}serde_json::from_value::<$name>(input).map_err(|e|e.to_string())?}};}
    match operation {
        "schema" => {
            request!(Request {});
            Ok(schema())
        }
        "validate" => {
            let r = request!(Request {
                model: String,
                value: Value
            });
            macro_rules! model {
                ($ty:ty) => {
                    output(parsed::<$ty>(r.value)?)
                };
            }
            match r.model.as_str() {
                "RoleReport" => model!(RoleReport),
                "RoleSpec" => model!(RoleSpec),
                "RoleVersion" => model!(RoleVersion),
                "RoleQuery" => model!(RoleQuery),
                "RoleResolution" => model!(RoleResolution),
                "RoleCalendar" => model!(RoleCalendar),
                "NextOpening" => model!(NextOpening),
                "ComputedAssignment" => model!(ComputedAssignment),
                "ComputedRoleIndex" => model!(ComputedRoleIndex),
                "ComputedRoleResolution" => model!(ComputedRoleResolution),
                _ => Err(format!("unsupported role model: {}", r.model)),
            }
        }
        "role_id" => {
            let r = request!(Request { spec: Value });
            output(RoleRegistry::from_value(r.spec)?.id())
        }
        "snapshot" => {
            let r = request!(Request { spec: Value });
            let registry = RoleRegistry::from_value(r.spec)?;
            output(RoleVersion {
                id: registry.id,
                spec: registry.spec,
            })
        }
        "resolve" => {
            let r = request!(Request {
                version: Value,
                query: Value
            });
            let version: RoleVersion = parsed(r.version)?;
            output(RoleRegistry::new(version.spec)?.resolve(&parsed(r.query)?)?)
        }
        "next_opening" => {
            let r = request!(Request { request: Value });
            output(next_opening(&parsed(r.request)?)?)
        }
        "resolve_computed" => {
            let r = request!(Request {
                index: Value,
                query: Value
            });
            output(ComputedRegistry::from_value(r.index)?.resolve(&parsed(r.query)?)?)
        }
        _ => Err(format!("unsupported role operation: {operation}")),
    }
}
#[cfg(test)]
mod tests;
