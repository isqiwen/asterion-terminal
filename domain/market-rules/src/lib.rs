//! Immutable market rules and explicit interpretation of settlement evidence.
//! This plugin owns no account state, persistence, source adapter or execution loop.
use asterion_foundation::decimal::Decimal;
use asterion_instrument_catalog::{Contract, Validate as CatalogValidate, validate_market_code};
use asterion_trading_calendar::{TimeVersion, Validate as CalendarValidate};
use chrono::{DateTime, Datelike, Duration, FixedOffset, NaiveDate, Timelike};
use regex::Regex;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::Value;
use std::sync::LazyLock;

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
fn connection(value: &Option<String>) -> Result<()> {
    static PATTERN: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^c_[0-9a-f]{32}$").unwrap());
    require(
        value.as_ref().is_none_or(|v| PATTERN.is_match(v)),
        "invalid connection_id",
    )
}
fn parsed<T: DeserializeOwned + Validate>(value: Value) -> Result<T> {
    let result: T = serde_json::from_value(value).map_err(|e| e.to_string())?;
    result.validate()?;
    Ok(result)
}
fn output(value: impl Serialize) -> Result<Value> {
    serde_json::to_value(value).map_err(|e| e.to_string())
}
fn required_option<'de, D: serde::Deserializer<'de>, T: Deserialize<'de>>(
    d: D,
) -> std::result::Result<Option<T>, D::Error> {
    Option::<T>::deserialize(d)
}
fn trimmed<'de, D: serde::Deserializer<'de>>(d: D) -> std::result::Result<String, D::Error> {
    Ok(String::deserialize(d)?.trim().into())
}
fn finite_stamp(value: DateTime<FixedOffset>) -> Result<()> {
    date(value.date_naive())?;
    require(
        value.nanosecond() < 1_000_000_000 && value.nanosecond().is_multiple_of(1000),
        "expected finite microsecond-precision datetime",
    )
}
mod stamp {
    use super::*;
    pub fn serialize<S: serde::Serializer>(
        value: &DateTime<FixedOffset>,
        serializer: S,
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
        serializer.serialize_str(&result)
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(
        d: D,
    ) -> std::result::Result<DateTime<FixedOffset>, D::Error> {
        let raw = String::deserialize(d)?;
        let value = DateTime::parse_from_rfc3339(&raw).map_err(serde::de::Error::custom)?;
        let value = value
            .with_nanosecond(value.nanosecond() / 1000 * 1000)
            .ok_or_else(|| serde::de::Error::custom("invalid datetime"))?;
        finite_stamp(value).map_err(serde::de::Error::custom)?;
        Ok(value)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FeeMode {
    PerLot,
    Notional,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FeeField {
    TradingFee,
    TradingFeeRate,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FeeUnit {
    YuanPerLot,
    Ratio,
    Percent,
    Permille,
    Permyriad,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum MarginUnit {
    Ratio,
    Percent,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FeeScope {
    LongOpenAndNonTodayClose,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum AvailabilityAssumption {
    AfterSourceDay,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SettlementRow {
    pub symbol: String,
    pub exchange: String,
    pub contract: String,
    pub trading_day: NaiveDate,
    #[serde(deserialize_with = "required_option")]
    pub settle: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub trading_fee_rate: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub trading_fee: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub delivery_fee: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub b_hedging_margin_rate: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub s_hedging_margin_rate: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub long_margin_rate: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub short_margin_rate: Option<Decimal>,
    #[serde(deserialize_with = "required_option")]
    pub offset_today_fee: Option<Decimal>,
}
impl Validate for SettlementRow {
    fn validate(&self) -> Result<()> {
        date(self.trading_day)?;
        for value in [
            &self.trading_fee_rate,
            &self.trading_fee,
            &self.delivery_fee,
            &self.b_hedging_margin_rate,
            &self.s_hedging_margin_rate,
            &self.long_margin_rate,
            &self.short_margin_rate,
            &self.offset_today_fee,
        ]
        .into_iter()
        .flatten()
        {
            require(
                value >= &Decimal::integer(0),
                "settlement fee and margin values must be nonnegative",
            )?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SettlementEvidence {
    #[schemars(length(min = 1, max = 100))]
    pub provider: String,
    #[schemars(length(min = 1, max = 100))]
    pub version_id: String,
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub checksum: String,
    #[serde(deserialize_with = "required_option")]
    pub connection_id: Option<String>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub observed_at: DateTime<FixedOffset>,
    pub row: SettlementRow,
    pub contract: Contract,
}
impl Validate for SettlementEvidence {
    fn validate(&self) -> Result<()> {
        length(&self.provider, 1, 100, "provider")?;
        length(&self.version_id, 1, 100, "version_id")?;
        fingerprint(&self.checksum)?;
        connection(&self.connection_id)?;
        finite_stamp(self.observed_at)?;
        self.row.validate()?;
        self.contract.validate()?;
        validate_market_code(&self.row.contract, &self.contract)?;
        require(
            self.contract.listed_on <= self.row.trading_day
                && self.row.trading_day <= self.contract.last_trade_on,
            "结算证据超出实际合约生命周期",
        )?;
        require(
            self.contract.product_id.split('.').next() == Some(self.row.exchange.as_str()),
            "结算证据交易所与身份不一致",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SettlementBasis {
    pub evidence: SettlementEvidence,
    pub fee_field: FeeField,
    pub fee_unit: FeeUnit,
    pub margin_unit: MarginUnit,
    pub fee_scope: FeeScope,
    pub availability_assumption: AvailabilityAssumption,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 1000))]
    pub interpretation: String,
}
impl SettlementBasis {
    pub fn new(mut model: Self) -> Result<Self> {
        model.interpretation = model.interpretation.trim().into();
        model.validate()?;
        Ok(model)
    }
    fn evidence_valid(&self) -> Result<()> {
        self.evidence.validate()?;
        length(&self.interpretation, 1, 1000, "interpretation")?;
        require(
            self.interpretation.trim() == self.interpretation,
            "interpretation must be trimmed",
        )?;
        require(
            (self.fee_field == FeeField::TradingFee) == (self.fee_unit == FeeUnit::YuanPerLot),
            "手续费字段与确认单位不一致",
        )
    }
    pub fn values(&self, precision: u32) -> Result<(FeeMode, Decimal, Decimal)> {
        self.evidence_valid()?;
        let row = &self.evidence.row;
        let fee = match self.fee_field {
            FeeField::TradingFee => row.trading_fee.as_ref(),
            FeeField::TradingFeeRate => row.trading_fee_rate.as_ref(),
        }
        .ok_or("选定手续费或买投机保证金缺失，不能采用该快照")?;
        let margin = row
            .long_margin_rate
            .as_ref()
            .ok_or("选定手续费或买投机保证金缺失，不能采用该快照")?;
        let power = match self.fee_unit {
            FeeUnit::YuanPerLot | FeeUnit::Ratio => 0,
            FeeUnit::Percent => 2,
            FeeUnit::Permille => 3,
            FeeUnit::Permyriad => 4,
        };
        let fee = fee.divide_power_ten(power, precision)?;
        let margin = margin.divide_power_ten(
            if self.margin_unit == MarginUnit::Percent {
                2
            } else {
                0
            },
            precision,
        )?;
        let mode = if self.fee_unit == FeeUnit::YuanPerLot {
            FeeMode::PerLot
        } else {
            FeeMode::Notional
        };
        require(
            margin > Decimal::integer(0)
                && margin <= Decimal::integer(1)
                && fee
                    <= Decimal::integer(if mode == FeeMode::PerLot {
                        1_000_000
                    } else {
                        1
                    }),
            "单位换算后费用或保证金超出有效范围",
        )?;
        Ok((mode, fee, margin))
    }
    /// Build a rule from already-frozen evidence. L3 must separately verify the
    /// referenced published version before admitting this rule into storage.
    pub fn period(&self, start: NaiveDate, end: NaiveDate, precision: u32) -> Result<RulePeriod> {
        let (fee_mode, fee, margin_rate) = self.values(precision)?;
        let period = RulePeriod {
            settlement_basis: Some(self.clone()),
            start,
            end,
            margin_rate,
            fee_mode,
            open_fee: fee.clone(),
            close_fee: fee,
        };
        period.validate()?;
        Ok(period)
    }
}
impl Validate for SettlementBasis {
    fn validate(&self) -> Result<()> {
        self.values(28).map(|_| ())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RulePeriod {
    #[serde(deserialize_with = "required_option")]
    pub settlement_basis: Option<SettlementBasis>,
    pub start: NaiveDate,
    pub end: NaiveDate,
    pub margin_rate: Decimal,
    pub fee_mode: FeeMode,
    pub open_fee: Decimal,
    pub close_fee: Decimal,
}
impl RulePeriod {
    pub fn new(mut model: Self) -> Result<Self> {
        if let Some(basis) = model.settlement_basis.take() {
            model.settlement_basis = Some(SettlementBasis::new(basis)?);
        }
        model.validate()?;
        Ok(model)
    }
    fn fee_validated(
        &self,
        price: &Decimal,
        multiplier: &Decimal,
        lots: i64,
        opening: bool,
        precision: u32,
    ) -> Result<Decimal> {
        let rate = if opening {
            &self.open_fee
        } else {
            &self.close_fee
        };
        let amount = rate.multiply(&Decimal::integer(lots), precision)?;
        let value = if self.fee_mode == FeeMode::Notional {
            price.multiply(multiplier, precision)?
        } else {
            Decimal::integer(1)
        };
        amount.multiply(&value, precision)
    }
    pub fn fee(
        &self,
        price: &Decimal,
        multiplier: &Decimal,
        lots: i64,
        opening: bool,
        precision: u32,
    ) -> Result<Decimal> {
        self.validate()?;
        self.fee_validated(price, multiplier, lots, opening, precision)
    }
}
impl Validate for RulePeriod {
    fn validate(&self) -> Result<()> {
        date(self.start)?;
        date(self.end)?;
        self.margin_rate.constraints(true, 1, 9, 8)?;
        self.open_fee.constraints(false, 1_000_000, 16, 8)?;
        self.close_fee.constraints(false, 1_000_000, 16, 8)?;
        require(self.end >= self.start, "规则生效日期范围无效")?;
        require(
            self.fee_mode != FeeMode::Notional
                || (self.open_fee <= Decimal::integer(1) && self.close_fee <= Decimal::integer(1)),
            "按成交金额收费的比例须在 0—1 之间",
        )?;
        if let Some(basis) = &self.settlement_basis {
            basis.validate()?;
            require(
                self.start > basis.evidence.row.trading_day,
                "盘后结算参数不能在其交易日或更早生效",
            )?;
            let (mode, fee, margin) = basis.values(28)?;
            require(
                self.fee_mode == mode
                    && self.open_fee == fee
                    && self.close_fee == fee
                    && self.margin_rate == margin,
                "规则参数与已确认的结算快照换算不一致",
            )?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ContractBasis {
    #[schemars(length(min = 1, max = 100))]
    pub provider: String,
    #[schemars(regex(pattern = r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+\.[0-9]{6}\.[0-9]{8}$"))]
    pub contract_id: String,
    #[schemars(length(min = 1, max = 100))]
    pub version_id: String,
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub checksum: String,
    #[serde(deserialize_with = "required_option")]
    pub connection_id: Option<String>,
    #[schemars(length(min = 1, max = 30))]
    pub symbol: String,
    #[schemars(length(min = 1, max = 10))]
    pub exchange: String,
    #[schemars(length(min = 1, max = 200))]
    pub name: String,
    pub listed: NaiveDate,
    #[serde(deserialize_with = "required_option")]
    pub delisted: Option<NaiveDate>,
    #[serde(deserialize_with = "required_option")]
    pub trade_unit: Option<String>,
    #[serde(deserialize_with = "required_option")]
    pub per_unit: Option<String>,
    #[serde(deserialize_with = "required_option")]
    pub multiplier: Option<String>,
    #[serde(deserialize_with = "required_option")]
    pub quote_unit: Option<String>,
    #[serde(deserialize_with = "required_option")]
    pub quote_unit_desc: Option<String>,
}
impl Validate for ContractBasis {
    fn validate(&self) -> Result<()> {
        length(&self.provider, 1, 100, "provider")?;
        length(&self.version_id, 1, 100, "version_id")?;
        fingerprint(&self.checksum)?;
        connection(&self.connection_id)?;
        length(&self.symbol, 1, 30, "symbol")?;
        length(&self.exchange, 1, 10, "exchange")?;
        length(&self.name, 1, 200, "name")?;
        static ID: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+\.[0-9]{6}\.[0-9]{8}$").unwrap()
        });
        require(ID.is_match(&self.contract_id), "invalid contract_id")?;
        date(self.listed)?;
        if let Some(value) = self.delisted {
            date(value)?;
        }
        require(
            self.contract_id.split('.').next() == Some(self.exchange.as_str()),
            "标准合约资料的交易所不一致",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RuleSpec {
    pub trading_time: TimeVersion,
    pub contract: Contract,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 100))]
    pub title: String,
    #[serde(deserialize_with = "trimmed")]
    #[schemars(length(min = 1, max = 1000))]
    pub source: String,
    pub multiplier: Decimal,
    pub tick_size: Decimal,
    #[serde(deserialize_with = "required_option")]
    pub basis: Option<ContractBasis>,
    #[schemars(length(min = 1, max = 100))]
    pub periods: Vec<RulePeriod>,
}
impl Validate for RuleSpec {
    fn validate(&self) -> Result<()> {
        self.trading_time.validate()?;
        self.contract.validate()?;
        length(&self.title, 1, 100, "title")?;
        length(&self.source, 1, 1000, "source")?;
        require(
            self.title.trim() == self.title && self.source.trim() == self.source,
            "rule title/source must be trimmed",
        )?;
        self.multiplier.constraints(true, 1_000_000, 16, 8)?;
        self.tick_size.constraints(true, 1_000_000, 16, 8)?;
        require(
            (1..=100).contains(&self.periods.len()),
            "periods: invalid length",
        )?;
        let time = &self.trading_time.spec;
        require(
            self.contract.product_id == format!("{}.{}", time.exchange.as_str(), time.product),
            "规则身份与交易时间品种不一致",
        )?;
        if let Some(basis) = &self.basis {
            basis.validate()?;
            require(
                self.contract.id == basis.contract_id
                    && self.contract.listed_on == basis.listed
                    && Some(self.contract.last_trade_on) == basis.delisted,
                "规则合约与来源资料不一致",
            )?;
        }
        for period in &self.periods {
            period.validate()?;
            if let Some(basis) = &period.settlement_basis {
                require(
                    basis.evidence.contract.id == self.contract.id,
                    "结算参数与规则合约不一致",
                )?;
            }
        }
        require(
            self.periods
                .windows(2)
                .all(|p| p[1].start - p[0].end == Duration::days(1)),
            "规则期间必须按日期排序、连续且不重叠",
        )
    }
}

/// Validated immutable rules. Queries share the fixed snapshot and use a binary
/// search over effective periods; no source lookup occurs during replay.
#[derive(Debug, Clone)]
pub struct Rules {
    spec: RuleSpec,
    id: String,
}
impl Rules {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(mut spec: RuleSpec) -> Result<Self> {
        spec.title = spec.title.trim().into();
        spec.source = spec.source.trim().into();
        for period in &mut spec.periods {
            if let Some(basis) = &mut period.settlement_basis {
                basis.interpretation = basis.interpretation.trim().into();
            }
        }
        spec.validate()?;
        let id = asterion_foundation::digest(&output(&spec)?)?;
        Ok(Self { spec, id })
    }
    pub fn spec(&self) -> &RuleSpec {
        &self.spec
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn at(&self, day: NaiveDate) -> Result<&RulePeriod> {
        date(day)?;
        let index = self
            .spec
            .periods
            .partition_point(|p| p.start <= day)
            .checked_sub(1)
            .ok_or("规则版本未覆盖所选日期")?;
        let period = &self.spec.periods[index];
        require(day <= period.end, "规则版本未覆盖所选日期")?;
        Ok(period)
    }
    pub fn cover(&self, contract_id: &str, start: NaiveDate, end: NaiveDate) -> Result<()> {
        date(start)?;
        date(end)?;
        require(
            self.spec.contract.id == contract_id,
            "规则版本与行情合约不一致",
        )?;
        require(
            self.spec.contract.listed_on <= start
                && start <= end
                && end <= self.spec.contract.last_trade_on,
            "规则请求超出固定合约生命周期",
        )?;
        self.at(start)?;
        self.at(end)?;
        Ok(())
    }
    pub fn fee(
        &self,
        day: NaiveDate,
        price: &Decimal,
        lots: i64,
        opening: bool,
        precision: u32,
    ) -> Result<Decimal> {
        self.at(day)?
            .fee_validated(price, &self.spec.multiplier, lots, opening, precision)
    }
}
pub fn rule_id(spec: &RuleSpec) -> Result<String> {
    Ok(Rules::new(spec.clone())?.id)
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RuleVersion {
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub id: String,
    pub spec: RuleSpec,
}
impl Validate for RuleVersion {
    fn validate(&self) -> Result<()> {
        fingerprint(&self.id)?;
        require(self.id == rule_id(&self.spec)?, "规则版本指纹不一致")
    }
}

pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[allow(dead_code)]
    #[serde(untagged)]
    enum Models {
        RulePeriod(RulePeriod),
        ContractBasis(ContractBasis),
        RuleSpec(RuleSpec),
        RuleVersion(RuleVersion),
        SettlementRow(SettlementRow),
        SettlementEvidence(SettlementEvidence),
        SettlementBasis(SettlementBasis),
    }
    let mut schema = output(schemars::schema_for!(Models)).expect("JSON schema is serializable");
    // Every nullable domain field is mandatory. Keeping Option's nullable branch
    // while adding required avoids schemars(required) narrowing it to non-null.
    for name in [
        "RulePeriod",
        "ContractBasis",
        "RuleSpec",
        "SettlementRow",
        "SettlementEvidence",
        "Contract",
    ] {
        let Some(object) = schema["$defs"][name].as_object_mut() else {
            continue;
        };
        let fields: Vec<Value> = object["properties"]
            .as_object()
            .unwrap()
            .keys()
            .cloned()
            .map(Value::String)
            .collect();
        object.insert("required".into(), Value::Array(fields));
    }
    for name in ["RuleSpec", "SettlementBasis"] {
        schema["$defs"][name]["x-strip-whitespace"] = Value::Bool(true);
    }
    for name in ["ContractBasis", "SettlementEvidence"] {
        schema["$defs"][name]["properties"]["connection_id"]["pattern"] =
            Value::String("^c_[0-9a-f]{32}$".into());
    }
    schema
}

pub fn invoke(operation: &str, input: Value) -> Result<Value> {
    macro_rules! request {($name:ident{$($field:ident:$ty:ty),* $(,)?})=>{{
        #[derive(Deserialize)] #[serde(deny_unknown_fields)] struct $name{$($field:$ty),*}
        serde_json::from_value::<$name>(input).map_err(|e|e.to_string())?
    }};}
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
                "RulePeriod" => model!(RulePeriod),
                "ContractBasis" => model!(ContractBasis),
                "RuleSpec" => model!(RuleSpec),
                "RuleVersion" => model!(RuleVersion),
                "SettlementRow" => model!(SettlementRow),
                "SettlementEvidence" => model!(SettlementEvidence),
                "SettlementBasis" => model!(SettlementBasis),
                _ => Err(format!("unsupported market rule model: {}", r.model)),
            }
        }
        "rule_id" => {
            let r = request!(Request { spec: Value });
            output(Rules::from_value(r.spec)?.id())
        }
        "snapshot" => {
            let r = request!(Request { spec: Value });
            let rules = Rules::from_value(r.spec)?;
            output(RuleVersion {
                id: rules.id,
                spec: rules.spec,
            })
        }
        "at" => {
            let r = request!(Request {
                spec: Value,
                day: NaiveDate
            });
            output(Rules::from_value(r.spec)?.at(r.day)?)
        }
        "cover" => {
            let r = request!(Request {
                spec: Value,
                contract_id: String,
                start: NaiveDate,
                end: NaiveDate
            });
            Rules::from_value(r.spec)?.cover(&r.contract_id, r.start, r.end)?;
            Ok(Value::Null)
        }
        "fee" => {
            let r = request!(Request {
                period: Value,
                price: Decimal,
                multiplier: Decimal,
                lots: i64,
                opening: bool,
                precision: u32
            });
            let period: RulePeriod = parsed(r.period)?;
            output(period.fee(&r.price, &r.multiplier, r.lots, r.opening, r.precision)?)
        }
        "settlement_values" => {
            let r = request!(Request {
                basis: Value,
                precision: u32
            });
            let basis: SettlementBasis = parsed(r.basis)?;
            output(basis.values(r.precision)?)
        }
        "settlement_period" => {
            let r = request!(Request {
                basis: Value,
                start: NaiveDate,
                end: NaiveDate,
                precision: u32
            });
            let basis: SettlementBasis = parsed(r.basis)?;
            output(basis.period(r.start, r.end, r.precision)?)
        }
        _ => Err(format!("unsupported market rule operation: {operation}")),
    }
}
#[cfg(test)]
mod tests;
