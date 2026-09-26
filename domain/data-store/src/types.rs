//! Built-in futures data types: their fixed manifests (identity, keys, time
//! semantics, fields) and row validation. Types are part of the domain; they
//! cannot be added or replaced at run time.
//!
//! Row values follow the lax scalar forms the data pipelines produce: decimals
//! as strings or JSON numbers, dates as `YYYY-MM-DD`, times as ISO 8601 with an
//! offset. Unknown row fields are ignored; missing required fields are invalid.
use chrono::{DateTime, FixedOffset, NaiveDate, NaiveDateTime, Timelike};
use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::collections::BTreeSet;
use std::sync::LazyLock;

pub type Result<T> = std::result::Result<T, String>;

const INVALID: &str = "数据类型校验失败：字段、数值或时间关系无效";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DataField {
    pub name: String,
    pub label: String,
    pub unit: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TypeManifest {
    pub id: String,
    pub label: String,
    pub domain: String,
    pub domain_label: String,
    pub shape: String,
    pub schema_version: i64,
    pub api_version: i64,
    pub frequency: String,
    pub primary_key: Vec<String>,
    pub time_field: Option<String>,
    pub time_semantics: String,
    pub fields: Vec<DataField>,
    pub description: String,
}

#[allow(clippy::too_many_arguments)]
fn manifest(
    id: &str,
    label: &str,
    domain: (&str, &str),
    shape: &str,
    schema_version: i64,
    frequency: &str,
    primary_key: &[&str],
    time_field: Option<&str>,
    time_semantics: &str,
    fields: &[(&str, &str, Option<&str>)],
    description: &str,
) -> TypeManifest {
    TypeManifest {
        id: id.into(),
        label: label.into(),
        domain: domain.0.into(),
        domain_label: domain.1.into(),
        shape: shape.into(),
        schema_version,
        api_version: 1,
        frequency: frequency.into(),
        primary_key: primary_key.iter().map(|k| (*k).into()).collect(),
        time_field: time_field.map(Into::into),
        time_semantics: time_semantics.into(),
        fields: fields
            .iter()
            .map(|(name, label, unit)| DataField {
                name: (*name).into(),
                label: (*label).into(),
                unit: unit.map(Into::into),
            })
            .collect(),
        description: description.into(),
    }
}

const REFERENCE: (&str, &str) = ("reference", "基础资料");
const MARKET: (&str, &str) = ("market", "行情");

/// Manifests of the built-in types, in their registration order.
pub fn manifests() -> Vec<TypeManifest> {
    vec![
        manifest(
            "futures.role_mapping",
            "供应商角色映射",
            REFERENCE,
            "table",
            1,
            "1d",
            &["product_id", "symbol", "role", "trading_day"],
            Some("trading_day"),
            "供应商交易日标签；历史公布时间可未知",
            &[
                ("exchange", "交易所", None),
                ("product_id", "品种", None),
                ("symbol", "角色来源代码", None),
                ("target_symbol", "目标来源代码", None),
                ("role", "角色", None),
                ("trading_day", "交易日", None),
                ("available_at", "历史可知时间", None),
            ],
            "供应商映射原始声明；须经固定合约资料解析为实际身份",
        ),
        manifest(
            "futures.contracts",
            "合约资料",
            REFERENCE,
            "table",
            4,
            "snapshot",
            &["exchange", "symbol", "listed"],
            None,
            "上市和到期区间；采集时间不代表规则生效时间",
            &[
                ("exchange", "交易所", None),
                ("symbol", "合约代码", None),
                ("name", "名称", None),
                ("product", "品种", None),
                ("currency", "币种", None),
                ("delivery_month", "交割年月", None),
                ("last_delivery_on", "最后交割日", None),
                ("listed", "上市日期", None),
                ("delisted", "最后交易日", None),
                ("trade_unit", "交易单位", None),
                ("per_unit", "每手数量", None),
                ("multiplier", "提供方合约乘数", None),
                ("quote_unit_desc", "最小报价说明", None),
                ("quote_unit", "报价单位", None),
                ("rules_status", "规则完整性", None),
                ("contract", "标准行情代码", None),
                ("suggested_multiplier", "建议每点每手乘数", None),
                ("multiplier_note", "乘数解释依据", None),
            ],
            "合约基础信息；不替代完整交易规则",
        ),
        manifest(
            "futures.calendar",
            "交易日历",
            REFERENCE,
            "table",
            1,
            "1d",
            &["exchange", "date"],
            Some("date"),
            "交易所当地日历日期；不包含日内交易时段",
            &[
                ("exchange", "交易所", None),
                ("date", "日期", None),
                ("is_open", "是否交易", None),
                ("previous_trading_day", "上一交易日", None),
            ],
            "交易日及休市日",
        ),
        manifest(
            "futures.settlement",
            "每日结算参数",
            REFERENCE,
            "table",
            1,
            "1d",
            &["contract", "trading_day"],
            Some("trading_day"),
            "盘后交易日快照；采集时间不是历史公布或规则生效时间",
            &[
                ("contract", "标准合约", None),
                ("symbol", "合约代码", None),
                ("exchange", "交易所", None),
                ("trading_day", "参数交易日", None),
                ("settle", "结算价", None),
                ("trading_fee_rate", "交易费率原值", None),
                ("trading_fee", "交易手续费原值", None),
                ("delivery_fee", "交割手续费原值", None),
                ("b_hedging_margin_rate", "买套保保证金原值", None),
                ("s_hedging_margin_rate", "卖套保保证金原值", None),
                ("long_margin_rate", "买投机保证金原值", None),
                ("short_margin_rate", "卖投机保证金原值", None),
                ("offset_today_fee", "平今费率原值", None),
            ],
            "提供方原始参数数值；未推断费率单位和开平仓适用性",
        ),
        manifest(
            "futures.daily",
            "历史日线",
            MARKET,
            "timeseries",
            1,
            "1d",
            &["contract", "trading_day"],
            Some("trading_day"),
            "交易日标签，不是日线可获知时刻",
            &[
                ("contract", "标准合约", None),
                ("exchange", "交易所", None),
                ("symbol", "合约代码", None),
                ("trading_day", "交易日", None),
                ("open", "开", None),
                ("high", "高", None),
                ("low", "低", None),
                ("close", "收", None),
                ("vol", "成交量", Some("手")),
                ("settle", "结算", None),
                ("pre_settle", "前结算", None),
                ("pre_close", "前收", None),
                ("oi", "持仓量", Some("手")),
                ("oi_chg", "持仓变化", Some("手")),
                ("amount", "成交额", Some("万元")),
            ],
            "实际期货合约日线、结算及持仓",
        ),
        manifest(
            "futures.minute",
            "期货分钟",
            MARKET,
            "timeseries",
            1,
            "specified",
            &["contract", "frequency", "event_time"],
            Some("event_time"),
            "Asia/Shanghai；固定时段解析交易日，时间戳含义由显式来源依据指定",
            &[
                ("contract", "实际合约代码", None),
                ("symbol", "来源代码", None),
                ("exchange", "交易所", None),
                ("trading_day", "交易日", None),
                ("event_time", "行情时间", None),
                ("frequency", "周期", None),
                ("available_at", "本地可获知时间", None),
                ("open", "开", None),
                ("high", "高", None),
                ("low", "低", None),
                ("close", "收", None),
                ("vol", "成交量", Some("手")),
                ("amount", "成交金额", Some("元")),
                ("oi", "持仓量", Some("手")),
            ],
            "实际月份合约供应商分钟数据，不补零、不推断历史可知时刻",
        ),
        manifest(
            "futures.bars",
            "历史行情 / 文件",
            MARKET,
            "timeseries",
            1,
            "unspecified",
            &["contract", "event_time"],
            Some("event_time"),
            "文件明确提供 event_time 与 available_at；频率未声明",
            &[
                ("contract", "标准合约", None),
                ("event_time", "事件时间", None),
                ("available_at", "可获知时间", None),
                ("trading_day", "交易日", None),
                ("open", "开", None),
                ("high", "高", None),
                ("low", "低", None),
                ("close", "收", None),
                ("volume", "成交量", Some("手")),
            ],
            "CSV 行情导入；不推断为日线，不与日线类型混合",
        ),
    ]
}

pub fn manifest_of(type_id: &str) -> Result<TypeManifest> {
    manifests()
        .into_iter()
        .find(|manifest| manifest.id == type_id)
        .ok_or_else(|| "未安装该数据类型插件".into())
}

// ----- lax scalar parsing (the published forms of the pipelines) -----

/// A finite decimal: its value for comparisons and its normalized digits.
#[derive(Debug, Clone, Copy)]
struct Decimal {
    value: f64,
    digits: u32,
    places: u32,
}

fn decimal(value: &Value) -> Option<Decimal> {
    let text = match value {
        Value::String(text) => text.trim().to_string(),
        Value::Number(number) => number.to_string(),
        _ => return None,
    };
    let cleaned = underscores(&text)?;
    let (mantissa, exponent) = match cleaned.find(['e', 'E']) {
        Some(at) => (&cleaned[..at], cleaned[at + 1..].parse::<i32>().ok()?),
        None => (cleaned.as_str(), 0),
    };
    let unsigned = mantissa.strip_prefix(['+', '-']).unwrap_or(mantissa);
    let (whole, fraction) = unsigned.split_once('.').unwrap_or((unsigned, ""));
    if (whole.is_empty() && fraction.is_empty())
        || !whole
            .bytes()
            .chain(fraction.bytes())
            .all(|b| b.is_ascii_digit())
    {
        return None;
    }
    let value: f64 = cleaned.parse().ok().filter(|v: &f64| v.is_finite())?;
    // Normalized coefficient digits and decimal places, as Decimal.normalize().
    let all: String = format!("{whole}{fraction}");
    let significant = all.trim_start_matches('0');
    let trailing = significant.len() - significant.trim_end_matches('0').len();
    let digits = significant.trim_end_matches('0').len().max(1) as i64;
    let exponent = i64::from(exponent) - fraction.len() as i64 + trailing as i64;
    let (digits, places) = if significant.is_empty() {
        (1, 0)
    } else if exponent >= 0 {
        (digits + exponent, 0)
    } else {
        (digits.max(-exponent), -exponent)
    };
    Some(Decimal {
        value,
        digits: digits as u32,
        places: places as u32,
    })
}

/// Python's numeric underscores: only between digits.
fn underscores(text: &str) -> Option<String> {
    let bytes = text.as_bytes();
    for (i, b) in bytes.iter().enumerate() {
        if *b == b'_'
            && !(i > 0
                && i + 1 < bytes.len()
                && bytes[i - 1].is_ascii_digit()
                && bytes[i + 1].is_ascii_digit())
        {
            return None;
        }
    }
    Some(text.replace('_', ""))
}

/// Pydantic's numeric time input: seconds, or milliseconds beyond 2e10,
/// rounded to microseconds.
fn timestamp(value: &Value) -> Option<DateTime<chrono::Utc>> {
    let number = match value {
        Value::Number(number) => number.as_f64()?,
        Value::String(text) => {
            // Plain decimal notation only: no whitespace or exponent.
            let digits = text.strip_prefix(['+', '-']).unwrap_or(text);
            let (whole, fraction) = digits.split_once('.').unwrap_or((digits, "0"));
            if whole.is_empty()
                || fraction.is_empty()
                || !whole
                    .bytes()
                    .chain(fraction.bytes())
                    .all(|b| b.is_ascii_digit())
            {
                return None;
            }
            text.parse::<f64>().ok()?
        }
        _ => return None,
    };
    if !number.is_finite() {
        return None;
    }
    let seconds = if number.abs() > 2e13 {
        number / 1e6
    } else if number.abs() > 2e10 {
        number / 1000.0
    } else {
        number
    };
    let micros = (seconds * 1e6).round();
    if micros.abs() >= 9e18 {
        return None;
    }
    DateTime::from_timestamp_micros(micros as i64).filter(|stamp| {
        use chrono::Datelike;
        (1..=9999).contains(&stamp.year())
    })
}

fn date(value: &Value) -> Option<NaiveDate> {
    if let Value::String(text) = value
        && let Ok(day) = NaiveDate::parse_from_str(text, "%Y-%m-%d")
        && text.len() == 10
    {
        return Some(day);
    }
    if let Value::String(text) = value
        && let Some(stamp) = naive(text)
    {
        // A datetime at exactly midnight is a date.
        return (stamp.time() == chrono::NaiveTime::MIN).then(|| stamp.date());
    }
    let stamp = timestamp(value)?;
    (stamp.time() == chrono::NaiveTime::MIN).then(|| stamp.date_naive())
}

fn normalized(text: &str) -> String {
    let mut text = text.replacen(' ', "T", 1);
    if let Some(at) = text.find('T') {
        let rest = &text[at + 1..];
        let clock_end = rest
            .find(|c: char| !(c.is_ascii_digit() || c == ':' || c == '.'))
            .unwrap_or(rest.len());
        if rest[..clock_end].matches(':').count() == 1 {
            text.insert_str(at + 1 + clock_end, ":00");
        }
    }
    text
}

fn naive(text: &str) -> Option<NaiveDateTime> {
    NaiveDateTime::parse_from_str(&normalized(text), "%Y-%m-%dT%H:%M:%S%.f").ok()
}

fn datetime(value: &Value) -> Option<DateTime<FixedOffset>> {
    if let Value::String(text) = value
        && let Ok(stamp) = DateTime::parse_from_rfc3339(&normalized(text))
    {
        return Some(stamp);
    }
    timestamp(value).map(|stamp| stamp.fixed_offset())
}

fn integer(value: &Value) -> Option<i64> {
    match value {
        Value::Bool(flag) => Some(i64::from(*flag)),
        Value::Number(number) => number.as_i64().or_else(|| {
            let float = number.as_f64()?;
            (float.fract() == 0.0).then_some(float as i64)
        }),
        Value::String(text) => {
            // Plain notation with an integral value; no exponent.
            if text.contains(['e', 'E']) {
                return None;
            }
            let decimal = decimal(&Value::String(text.clone()))?;
            (decimal.places == 0).then_some(decimal.value as i64)
        }
        _ => None,
    }
}

/// Field access over one row; every failure is the same user-facing refusal.
struct Fields<'a>(&'a Map<String, Value>);

fn required<T>(value: Option<T>) -> Result<T> {
    value.ok_or_else(|| INVALID.into())
}

fn check(ok: bool) -> Result<()> {
    if ok { Ok(()) } else { Err(INVALID.into()) }
}

impl Fields<'_> {
    fn get(&self, name: &str) -> Result<&Value> {
        required(self.0.get(name))
    }
    /// Present or defaulted-to-null optional field.
    fn optional(&self, name: &str) -> &Value {
        self.0.get(name).unwrap_or(&Value::Null)
    }
    fn text(&self, name: &str) -> Result<&str> {
        let text = required(self.get(name)?.as_str())?;
        check(!text.is_empty())?;
        Ok(text)
    }
    fn nullable_text(&self, name: &str) -> Result<Option<&str>> {
        match self.get(name)? {
            Value::Null => Ok(None),
            value => required(value.as_str()).map(Some),
        }
    }
    fn decimal(&self, name: &str) -> Result<Decimal> {
        required(decimal(self.get(name)?))
    }
    fn nullable_decimal(&self, value: &Value) -> Result<Option<Decimal>> {
        match value {
            Value::Null => Ok(None),
            value => required(decimal(value)).map(Some),
        }
    }
    fn date(&self, name: &str) -> Result<NaiveDate> {
        required(date(self.get(name)?))
    }
    fn nullable_date(&self, name: &str) -> Result<Option<NaiveDate>> {
        match self.get(name)? {
            Value::Null => Ok(None),
            value => required(date(value)).map(Some),
        }
    }
    fn datetime(&self, name: &str) -> Result<DateTime<FixedOffset>> {
        required(datetime(self.get(name)?))
    }
}

static PRODUCT: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[A-Z]+$").expect("pattern"));
static CURRENCY: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[A-Z]{3}$").expect("pattern"));
static MONTH: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^[0-9]{4}-(0[1-9]|1[0-2])$").expect("pattern"));
static PRODUCT_ID: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+$").expect("pattern"));
static BAR_CONTRACT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^(?:(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}|SIM\.DEMO001)$")
        .expect("pattern")
});

fn ohlc(open: Decimal, high: Decimal, low: Decimal, close: Decimal) -> Result<()> {
    check(low.value <= open.value.min(close.value) && high.value >= open.value.max(close.value))
}

fn contract(row: &Fields) -> Result<()> {
    row.text("contract")?;
    let suggested = row.nullable_decimal(row.get("suggested_multiplier")?)?;
    check(suggested.is_none_or(|v| v.value > 0.0 && v.value <= 1e6))?;
    row.text("multiplier_note")?;
    row.text("exchange")?;
    row.text("symbol")?;
    row.text("name")?;
    check(PRODUCT.is_match(required(row.get("product")?.as_str())?))?;
    check(CURRENCY.is_match(required(row.get("currency")?.as_str())?))?;
    if let Some(month) = row.nullable_text("delivery_month")? {
        check(MONTH.is_match(month))?;
    }
    let last_delivery = row.nullable_date("last_delivery_on")?;
    let listed = row.date("listed")?;
    let delisted = row.nullable_date("delisted")?;
    row.nullable_decimal(row.get("multiplier")?)?;
    row.nullable_text("quote_unit_desc")?;
    if let (Some(last), Some(delisted)) = (last_delivery, delisted) {
        check(last >= delisted)?;
    }
    check(delisted.is_none_or(|d| d >= listed))
}

fn minute(row: &Fields) -> Result<()> {
    check(matches!(
        required(row.get("frequency")?.as_str())?,
        "1m" | "5m" | "15m" | "30m" | "60m"
    ))?;
    for name in ["contract", "symbol", "exchange"] {
        required(row.get(name)?.as_str())?;
    }
    row.date("trading_day")?;
    let event = row.datetime("event_time")?;
    let available = row.datetime("available_at")?;
    let (open, high, low, close) = (
        row.decimal("open")?,
        row.decimal("high")?,
        row.decimal("low")?,
        row.decimal("close")?,
    );
    for name in ["vol", "amount", "oi"] {
        check(row.decimal(name)?.value >= 0.0)?;
    }
    ohlc(open, high, low, close)?;
    check(available >= event && event.second() == 0 && event.nanosecond() == 0)
}

fn calendar(row: &Fields) -> Result<()> {
    row.text("exchange")?;
    let day = row.date("date")?;
    let open = required(integer(row.get("is_open")?))?;
    check((0..=1).contains(&open))?;
    let previous = row.nullable_date("previous_trading_day")?;
    check(previous.is_none_or(|p| p < day))
}

fn role_mapping(row: &Fields) -> Result<()> {
    let exchange = row.text("exchange")?;
    let product = required(row.get("product_id")?.as_str())?;
    check(PRODUCT_ID.is_match(product))?;
    row.text("symbol")?;
    row.text("target_symbol")?;
    check(matches!(
        required(row.get("role")?.as_str())?,
        "main" | "secondary"
    ))?;
    row.date("trading_day")?;
    match row.get("available_at")? {
        Value::Null => {}
        value => {
            required(datetime(value))?;
        }
    }
    check(product.split('.').next() == Some(exchange))
}

fn settlement(row: &Fields) -> Result<()> {
    for name in ["exchange", "symbol", "contract"] {
        row.text(name)?;
    }
    row.date("trading_day")?;
    row.nullable_decimal(row.get("settle")?)?;
    for name in [
        "trading_fee_rate",
        "trading_fee",
        "delivery_fee",
        "b_hedging_margin_rate",
        "s_hedging_margin_rate",
        "long_margin_rate",
        "short_margin_rate",
        "offset_today_fee",
    ] {
        let value = row.nullable_decimal(row.get(name)?)?;
        check(value.is_none_or(|v| v.value >= 0.0))?;
    }
    Ok(())
}

fn daily(row: &Fields) -> Result<()> {
    row.nullable_decimal(row.optional("settle"))?;
    for name in ["exchange", "symbol", "contract"] {
        row.text(name)?;
    }
    row.date("trading_day")?;
    let (open, high, low, close) = (
        row.decimal("open")?,
        row.decimal("high")?,
        row.decimal("low")?,
        row.decimal("close")?,
    );
    let volume = row.decimal("vol")?;
    check(volume.value >= 0.0 && volume.places == 0)?;
    for name in ["amount", "oi"] {
        let value = row.nullable_decimal(row.optional(name))?;
        check(value.is_none_or(|v| v.value >= 0.0))?;
    }
    ohlc(open, high, low, close)
}

fn price(row: &Fields, name: &str) -> Result<Decimal> {
    let value = row.decimal(name)?;
    check(value.value > 0.0 && value.digits <= 20 && value.places <= 8)?;
    Ok(value)
}

fn bar(row: &Fields) -> Result<()> {
    check(BAR_CONTRACT.is_match(required(row.get("contract")?.as_str())?))?;
    let event = row.datetime("event_time")?;
    let available = row.datetime("available_at")?;
    row.date("trading_day")?;
    let (open, high, low, close) = (
        price(row, "open")?,
        price(row, "high")?,
        price(row, "low")?,
        price(row, "close")?,
    );
    check(required(integer(row.get("volume")?))? >= 0)?;
    check(event.nanosecond() == 0)?;
    ohlc(open, high, low, close)?;
    check(available >= event)
}

/// Python's `str()` of a JSON scalar, the form primary keys are compared in.
fn key_text(value: &Value) -> String {
    match value {
        Value::Null => "None".into(),
        Value::Bool(true) => "True".into(),
        Value::Bool(false) => "False".into(),
        Value::String(text) => text.clone(),
        Value::Number(number) if number.is_f64() => {
            let float = number.as_f64().unwrap_or_default();
            if float.fract() == 0.0 && float.abs() < 1e16 {
                format!("{float:.1}")
            } else {
                float.to_string()
            }
        }
        other => other.to_string(),
    }
}

/// Validate rows of a type, then refuse duplicate primary keys.
pub fn validate(type_id: &str, rows: &[Value]) -> Result<()> {
    let manifest = manifest_of(type_id)?;
    let check_row: fn(&Fields) -> Result<()> = match type_id {
        "futures.contracts" => contract,
        "futures.minute" => minute,
        "futures.calendar" => calendar,
        "futures.role_mapping" => role_mapping,
        "futures.settlement" => settlement,
        "futures.daily" => daily,
        "futures.bars" => bar,
        _ => return Err("未安装该数据类型插件".into()),
    };
    let mut keys = BTreeSet::new();
    let mut duplicate = false;
    for row in rows {
        let fields = required(row.as_object())?;
        check_row(&Fields(fields))?;
        let key: Vec<String> = manifest
            .primary_key
            .iter()
            .map(|field| fields.get(field).map_or_else(|| "None".into(), key_text))
            .collect();
        duplicate |= !keys.insert(key);
    }
    if duplicate {
        return Err("返回了重复数据键，未发布".into());
    }
    Ok(())
}

/// How completely the rows cover the requested range.
pub fn coverage(
    type_id: &str,
    rows: usize,
    start: Option<NaiveDate>,
    end: Option<NaiveDate>,
) -> Result<&'static str> {
    manifest_of(type_id)?;
    if type_id != "futures.calendar" {
        return Ok("RETURNED_ROWS_ONLY");
    }
    match (start, end) {
        (Some(start), Some(end)) if rows as i64 == (end - start).num_days() + 1 => {
            Ok("CALENDAR_COMPLETE")
        }
        _ => Err("交易日历缺少日期，未发布".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn daily_row() -> Value {
        json!({"exchange": "SHFE", "symbol": "rb2610", "contract": "SHFE.rb2610",
               "trading_day": "2026-01-05", "open": "10", "high": "12", "low": "9",
               "close": "11", "vol": "100", "extra": "ignored"})
    }

    #[test]
    fn decimals_follow_the_lax_published_forms() {
        for (text, valid) in [
            ("1.5", true),
            (" 1.5 ", true),
            ("1e2", true),
            ("+1", true),
            ("1_000", true),
            ("NaN", false),
            ("inf", false),
            ("", false),
            ("abc", false),
            ("1__0", false),
        ] {
            assert_eq!(decimal(&json!(text)).is_some(), valid, "{text}");
        }
        assert!(decimal(&json!(true)).is_none());
        let places = |text: &str| decimal(&json!(text)).unwrap().places;
        assert_eq!(
            (
                places("10.0"),
                places("10.50"),
                places("1e1"),
                places("0.0")
            ),
            (0, 1, 0, 0)
        );
        assert_eq!(decimal(&json!("123.45000000")).unwrap().digits, 5);
    }

    #[test]
    fn dates_and_times_accept_their_published_forms_only() {
        assert!(date(&json!("2024-01-02")).is_some());
        assert!(date(&json!("2024-01-02T00:00:00")).is_some());
        for text in ["2024-1-2", "2024-01-02T01:00:00", "20240102", "2024-01-02 "] {
            assert!(date(&json!(text)).is_none(), "{text}");
        }
        for text in [
            "2024-01-02T00:00:00+08:00",
            "2024-01-02T00:00:00Z",
            "2024-01-02 00:00:00+08:00",
            "2024-01-02T00:00+08:00",
        ] {
            assert!(datetime(&json!(text)).is_some(), "{text}");
        }
        for text in ["2024-01-02T00:00:00", "2024-01-02"] {
            assert!(datetime(&json!(text)).is_none(), "{text}");
        }
        assert_eq!(
            (
                integer(&json!("1.0")),
                integer(&json!(true)),
                integer(&json!(1.5))
            ),
            (Some(1), Some(1), None)
        );
    }

    #[test]
    fn rows_are_checked_per_type_and_keys_are_unique() {
        validate("futures.daily", &[daily_row()]).unwrap();
        for (field, value) in [
            ("vol", json!("10.5")),
            ("low", json!("12")),
            ("vol", json!("-1")),
            ("open", json!(null)),
        ] {
            let mut row = daily_row();
            row[field] = value;
            assert_eq!(
                validate("futures.daily", &[row]).unwrap_err(),
                INVALID,
                "{field}"
            );
        }
        let mut row = daily_row();
        row.as_object_mut().unwrap().remove("close");
        assert!(validate("futures.daily", &[row]).is_err());
        assert_eq!(
            validate("futures.daily", &[daily_row(), daily_row()]).unwrap_err(),
            "返回了重复数据键，未发布"
        );
        assert!(validate("testing.other", &[]).is_err());
        let day = json!({"exchange": "SHFE", "date": "2026-01-05", "is_open": 1, "previous_trading_day": "2026-01-02"});
        validate("futures.calendar", std::slice::from_ref(&day)).unwrap();
        let mut late = day.clone();
        late["previous_trading_day"] = json!("2026-01-05");
        assert!(validate("futures.calendar", &[late]).is_err());
        let mut missing = day;
        missing
            .as_object_mut()
            .unwrap()
            .remove("previous_trading_day");
        assert!(validate("futures.calendar", &[missing]).is_err());
        let start = NaiveDate::from_ymd_opt(2026, 1, 1);
        let end = NaiveDate::from_ymd_opt(2026, 1, 3);
        assert_eq!(
            coverage("futures.calendar", 3, start, end).unwrap(),
            "CALENDAR_COMPLETE"
        );
        assert!(coverage("futures.calendar", 2, start, end).is_err());
        assert_eq!(
            coverage("futures.daily", 0, None, None).unwrap(),
            "RETURNED_ROWS_ONLY"
        );
        assert_eq!(manifests().len(), 7);
        assert_eq!(manifest_of("futures.contracts").unwrap().schema_version, 4);
    }
}
