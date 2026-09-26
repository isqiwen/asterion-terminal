//! Explicit product calendars and session rules. No weekday/holiday inference.
//! Validated calendars cache their session expansion and lookup indexes.

use chrono::{
    DateTime, Datelike, Duration, FixedOffset, LocalResult, NaiveDate, NaiveDateTime, NaiveTime,
    TimeZone, Timelike,
};
use chrono_tz::Asia::Shanghai;
use regex::Regex;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
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
fn valid_date(date: NaiveDate) -> Result<()> {
    require(
        (1..=9999).contains(&date.year()),
        "date year must be in 1..9999",
    )
}
fn valid_stamp(stamp: DateTime<FixedOffset>) -> Result<()> {
    valid_date(stamp.date_naive())?;
    require(
        stamp.nanosecond() < 1_000_000_000,
        "leap seconds are unsupported",
    )?;
    require(
        stamp.nanosecond().is_multiple_of(1000),
        "datetime precision must not exceed microseconds",
    )
}
fn parsed<T: DeserializeOwned + Validate>(value: Value) -> Result<T> {
    let model: T = serde_json::from_value(value).map_err(|e| e.to_string())?;
    model.validate()?;
    Ok(model)
}
fn output<T: Serialize>(value: T) -> Result<Value> {
    serde_json::to_value(value).map_err(|e| e.to_string())
}
mod stamp {
    use chrono::{DateTime, FixedOffset, Timelike};
    use serde::{Deserialize, Deserializer, Serializer};
    pub fn serialize<S: Serializer>(
        value: &DateTime<FixedOffset>,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        let mut result = value.format("%Y-%m-%dT%H:%M:%S").to_string();
        if value.nanosecond() != 0 {
            result.push_str(&format!(".{:06}", value.nanosecond() / 1000));
        }
        let offset = value.offset().local_minus_utc();
        if offset == 0 {
            result.push('Z');
        } else {
            // The wire contract truncates historical sub-minute offsets.
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
    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<DateTime<FixedOffset>, D::Error> {
        let value = String::deserialize(deserializer)?;
        let parsed = DateTime::parse_from_rfc3339(&value).map_err(serde::de::Error::custom)?;
        let parsed = parsed
            .with_nanosecond(parsed.nanosecond() / 1000 * 1000)
            .ok_or_else(|| serde::de::Error::custom("invalid datetime"))?;
        super::valid_stamp(parsed).map_err(serde::de::Error::custom)?;
        Ok(parsed)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub enum Exchange {
    SHFE,
    DCE,
    CZCE,
    CFFEX,
    INE,
    GFEX,
}
impl Exchange {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::SHFE => "SHFE",
            Self::DCE => "DCE",
            Self::CZCE => "CZCE",
            Self::CFFEX => "CFFEX",
            Self::INE => "INE",
            Self::GFEX => "GFEX",
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    Auction,
    Continuous,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Session {
    Day,
    Night,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Boundary {
    Event,
    BarEnd,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum BarBoundary {
    BarStart,
    BarEnd,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CalendarDay {
    pub date: NaiveDate,
    pub is_open: bool,
    pub night_open: bool,
}
impl Validate for CalendarDay {
    fn validate(&self) -> Result<()> {
        valid_date(self.date)?;
        require(
            !self.night_open || self.is_open,
            "夜盘起始自然日必须是日历开市日",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Slot {
    #[schemars(regex(pattern = r"^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$"))]
    pub start: String,
    #[schemars(regex(pattern = r"^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$"))]
    pub end: String,
    #[schemars(range(min = 0, max = 1))]
    pub end_offset: u8,
    pub phase: Phase,
}
fn clock(value: &str) -> Result<NaiveTime> {
    let bytes = value.as_bytes();
    require(
        bytes.len() == 8
            && bytes[2] == b':'
            && bytes[5] == b':'
            && [0, 1, 3, 4, 6, 7]
                .iter()
                .all(|i| bytes[*i].is_ascii_digit()),
        "invalid session clock",
    )?;
    let time = NaiveTime::parse_from_str(value, "%H:%M:%S").map_err(|e| e.to_string())?;
    require(time.nanosecond() == 0, "invalid session clock")?;
    Ok(time)
}
impl Validate for Slot {
    fn validate(&self) -> Result<()> {
        clock(&self.start)?;
        clock(&self.end)?;
        require(self.end_offset <= 1, "end_offset must be 0 or 1")?;
        require(
            self.end_offset != 0 || self.end > self.start,
            "时段结束必须晚于开始",
        )?;
        require(
            self.end_offset != 1 || self.end <= self.start,
            "单个交易时段不得超过 24 小时",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Period {
    pub start: NaiveDate,
    pub end: NaiveDate,
    #[schemars(length(min = 1, max = 2000))]
    pub source: String,
    #[schemars(length(min = 1, max = 20))]
    pub day: Vec<Slot>,
    #[schemars(length(max = 20))]
    pub night: Vec<Slot>,
}
fn slots(values: &[Slot], min: usize) -> Result<()> {
    require(
        (min..=20).contains(&values.len()),
        "session slots: invalid length",
    )?;
    for slot in values {
        slot.validate()?;
    }
    Ok(())
}
impl Validate for Period {
    fn validate(&self) -> Result<()> {
        valid_date(self.start)?;
        valid_date(self.end)?;
        length(&self.source, 1, 2000, "source")?;
        slots(&self.day, 1)?;
        slots(&self.night, 0)?;
        require(
            self.end >= self.start && self.day.iter().all(|s| s.end_offset == 0),
            "日盘日期或时段无效",
        )?;
        require(
            self.day.iter().any(|s| s.phase == Phase::Continuous),
            "日盘须包含连续交易时段",
        )
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ExceptionDay {
    pub trading_day: NaiveDate,
    #[schemars(length(min = 1, max = 2000))]
    pub source: String,
    #[schemars(length(max = 20))]
    pub day: Vec<Slot>,
    #[schemars(length(max = 20))]
    pub night: Vec<Slot>,
}
impl Validate for ExceptionDay {
    fn validate(&self) -> Result<()> {
        valid_date(self.trading_day)?;
        length(&self.source, 1, 2000, "source")?;
        slots(&self.day, 0)?;
        slots(&self.night, 0)
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TimeSpec {
    #[schemars(range(min = 1, max = 1))]
    pub schema_version: u8,
    pub exchange: Exchange,
    #[schemars(length(max = 20), regex(pattern = r"^[A-Z]+$"))]
    pub product: String,
    #[schemars(length(min = 1, max = 100))]
    pub title: String,
    #[schemars(regex(pattern = r"^Asia/Shanghai$"))]
    pub timezone: String,
    #[schemars(length(min = 1, max = 2000))]
    pub calendar_source: String,
    #[schemars(length(min = 1, max = 2000))]
    pub night_source: String,
    #[schemars(length(min = 2, max = 3700))]
    pub calendar: Vec<CalendarDay>,
    #[schemars(length(min = 1, max = 100))]
    pub periods: Vec<Period>,
    #[schemars(length(max = 200))]
    pub exceptions: Vec<ExceptionDay>,
}
impl TimeSpec {
    fn validate_rules(&self) -> Result<()> {
        require(
            self.schema_version == 1,
            "unsupported trading time schema_version",
        )?;
        length(&self.product, 1, 20, "product")?;
        require(
            self.product.bytes().all(|c| c.is_ascii_uppercase()),
            "invalid product",
        )?;
        length(&self.title, 1, 100, "title")?;
        require(self.timezone == "Asia/Shanghai", "unsupported timezone")?;
        length(&self.calendar_source, 1, 2000, "calendar_source")?;
        length(&self.night_source, 1, 2000, "night_source")?;
        require(
            (2..=3700).contains(&self.calendar.len()),
            "calendar: invalid length",
        )?;
        require(
            (1..=100).contains(&self.periods.len()),
            "periods: invalid length",
        )?;
        require(self.exceptions.len() <= 200, "exceptions: invalid length")?;
        for day in &self.calendar {
            day.validate()?;
        }
        for period in &self.periods {
            period.validate()?;
        }
        for exception in &self.exceptions {
            exception.validate()?;
        }
        require(
            self.calendar
                .windows(2)
                .all(|p| p[1].date - p[0].date == Duration::days(1)),
            "自然日日历必须连续、排序且无重复；不能省略休市日",
        )?;
        require(
            self.periods
                .windows(2)
                .all(|p| p[1].start - p[0].end == Duration::days(1)),
            "品种时段生效期间必须连续、排序且无重叠",
        )?;
        let first = &self.periods[0];
        let last = &self.periods[self.periods.len() - 1];
        require(
            first.start > self.calendar[0].date
                && last.end <= self.calendar[self.calendar.len() - 1].date,
            "日历须包含规则开始前的开市日及完整生效期间",
        )?;
        require(
            self.calendar
                .iter()
                .any(|d| d.is_open && d.date < first.start),
            "缺少起始交易日前的开市日依据",
        )?;
        let mut exceptions = BTreeSet::new();
        for e in &self.exceptions {
            require(exceptions.insert(e.trading_day), "特殊时段日期重复")?;
            require(
                first.start <= e.trading_day && e.trading_day <= last.end,
                "特殊时段超出规则范围",
            )?;
            require(
                e.day.iter().all(|s| s.end_offset == 0),
                "特殊日盘不能跨自然日",
            )?;
            let index = (e.trading_day - self.calendar[0].date).num_days() as usize;
            require(self.calendar[index].is_open, "休市交易日不能定义交易时段")?;
        }
        Ok(())
    }
    fn check_contract(&self, contract: &str) -> Result<()> {
        static CODE: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^([A-Z]+)\.([A-Za-z]+)[0-9]{3,4}$").unwrap());
        let matched = CODE.captures(contract).is_some_and(|c| {
            &c[1] == self.exchange.as_str() && c[2].to_ascii_uppercase() == self.product
        });
        require(matched, "交易时间版本与合约交易所或品种不一致")
    }
}
impl Validate for TimeSpec {
    fn validate(&self) -> Result<()> {
        Calendar::new(self.clone()).map(|_| ())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Span {
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub start: DateTime<FixedOffset>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub end: DateTime<FixedOffset>,
    pub trading_day: NaiveDate,
    pub session: Session,
    pub phase: Phase,
}
impl Validate for Span {
    fn validate(&self) -> Result<()> {
        valid_stamp(self.start)?;
        valid_stamp(self.end)?;
        valid_date(self.trading_day)
    }
}

// ZoneInfo's default fold=0 uses the earlier offset at a repeated wall time and
// the pre-transition offset in a gap. Keep that choice for legal historical specs.
fn local(value: NaiveDateTime) -> Result<DateTime<FixedOffset>> {
    match Shanghai.from_local_datetime(&value) {
        LocalResult::Single(stamp) => Ok(stamp.fixed_offset()),
        LocalResult::Ambiguous(first, _) => Ok(first.fixed_offset()),
        LocalResult::None => {
            for hours in 1..=48 {
                let earlier = value
                    .checked_sub_signed(Duration::hours(hours))
                    .ok_or("session datetime overflow")?;
                if let Some(before) = Shanghai.from_local_datetime(&earlier).earliest() {
                    return before
                        .fixed_offset()
                        .offset()
                        .from_local_datetime(&value)
                        .single()
                        .ok_or_else(|| "invalid session datetime".into());
                }
            }
            Err("无法解析交易所本地时区".into())
        }
    }
}
fn expanded(spec: &TimeSpec) -> Result<Vec<Span>> {
    let mut values = Vec::new();
    let mut previous: Option<&CalendarDay> = None;
    let exceptions: BTreeMap<NaiveDate, &ExceptionDay> =
        spec.exceptions.iter().map(|e| (e.trading_day, e)).collect();
    for item in &spec.calendar {
        if !item.is_open {
            continue;
        }
        let period_index = spec.periods.partition_point(|p| p.start <= item.date);
        if let Some(period) = period_index
            .checked_sub(1)
            .map(|i| &spec.periods[i])
            .filter(|p| item.date <= p.end)
        {
            let (day, night) = exceptions
                .get(&item.date)
                .map_or((&period.day, &period.night), |e| (&e.day, &e.night));
            for (slots, anchor, session) in [
                (day, Some(item.date), Session::Day),
                (night, previous.map(|d| d.date), Session::Night),
            ] {
                if session == Session::Night && !previous.is_some_and(|d| d.night_open) {
                    continue;
                }
                let anchor = anchor.ok_or("缺少夜盘锚定自然日")?;
                for slot in slots {
                    let end_date = anchor
                        .checked_add_signed(Duration::days(i64::from(slot.end_offset)))
                        .ok_or("session date overflow")?;
                    valid_date(end_date)?;
                    let start = local(anchor.and_time(clock(&slot.start)?))?;
                    let end = local(end_date.and_time(clock(&slot.end)?))?;
                    require(
                        session != Session::Night
                            || end.naive_local() < item.date.and_hms_opt(9, 0, 0).unwrap(),
                        "夜盘不得越过目标交易日日盘边界",
                    )?;
                    values.push(Span {
                        start,
                        end,
                        trading_day: item.date,
                        session,
                        phase: slot.phase,
                    });
                }
            }
        }
        previous = Some(item);
    }
    values.sort_by_key(|s| s.start.naive_local());
    require(
        values
            .windows(2)
            .all(|p| p[0].end.naive_local() <= p[1].start.naive_local()),
        "交易时段重叠或跨交易日冲突",
    )?;
    Ok(values)
}

/// Cache this immutable object at the language boundary, rather than rebuilding
/// a multiyear calendar for every bar. Daily lookup is indexed; resolve is O(log n).
#[derive(Debug, Clone)]
pub struct Calendar {
    spec: TimeSpec,
    id: String,
    spans: Vec<Span>,
    by_day: BTreeMap<NaiveDate, Vec<usize>>,
}
impl Calendar {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(spec: TimeSpec) -> Result<Self> {
        spec.validate_rules()?;
        let spans = expanded(&spec)?;
        let mut by_day: BTreeMap<NaiveDate, Vec<usize>> = BTreeMap::new();
        for (index, span) in spans.iter().enumerate() {
            if span.phase == Phase::Continuous {
                by_day.entry(span.trading_day).or_default().push(index);
            }
        }
        let id = asterion_foundation::digest(&output(&spec)?)?;
        Ok(Self {
            spec,
            id,
            spans,
            by_day,
        })
    }
    pub fn spec(&self) -> &TimeSpec {
        &self.spec
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn spans(&self) -> &[Span] {
        &self.spans
    }
    pub fn check_contract(&self, contract: &str) -> Result<()> {
        self.spec.check_contract(contract)
    }
    pub fn daily(&self, contract: &str, day: NaiveDate) -> Result<Vec<&Span>> {
        self.check_contract(contract)?;
        valid_date(day)?;
        require(
            self.spec.periods[0].start <= day
                && day <= self.spec.periods[self.spec.periods.len() - 1].end,
            "交易时间版本未覆盖交易日",
        )?;
        let indexes = self
            .by_day
            .get(&day)
            .ok_or("交易日休市或品种停盘，不能接收行情")?;
        Ok(indexes.iter().map(|i| &self.spans[*i]).collect())
    }
    pub fn resolve(
        &self,
        contract: &str,
        stamp: DateTime<FixedOffset>,
        boundary: Boundary,
    ) -> Result<&Span> {
        self.check_contract(contract)?;
        self.span_at(stamp, boundary)
    }
    /// Resolve a timestamp in this product calendar without inventing a provider
    /// market code. Callers own their canonical instrument/product identity.
    pub fn span_at(&self, stamp: DateTime<FixedOffset>, boundary: Boundary) -> Result<&Span> {
        valid_stamp(stamp)?;
        let wall = stamp.with_timezone(&Shanghai).naive_local();
        let position = self.spans.partition_point(|span| match boundary {
            Boundary::Event => span.start.naive_local() <= wall,
            Boundary::BarEnd => span.start.naive_local() < wall,
        });
        let span = position
            .checked_sub(1)
            .map(|i| &self.spans[i])
            .ok_or("时间不在已确认的交易时段内，或缺少时间规则覆盖")?;
        let contains = match boundary {
            Boundary::Event => wall < span.end.naive_local(),
            Boundary::BarEnd => wall <= span.end.naive_local() && span.phase == Phase::Continuous,
        };
        require(contains, "时间不在已确认的交易时段内，或缺少时间规则覆盖")?;
        Ok(span)
    }
    pub fn validate_bar(
        &self,
        contract: &str,
        stamp: DateTime<FixedOffset>,
        day: NaiveDate,
        seconds: i64,
        boundary: BarBoundary,
    ) -> Result<&Span> {
        valid_date(day)?;
        require(seconds > 0, "行情周期秒数必须为正数")?;
        let span = self.resolve(
            contract,
            stamp,
            if boundary == BarBoundary::BarEnd {
                Boundary::BarEnd
            } else {
                Boundary::Event
            },
        )?;
        let duration = Duration::try_seconds(seconds).ok_or("bar duration overflow")?;
        let start = if boundary == BarBoundary::BarEnd {
            stamp
                .checked_sub_signed(duration)
                .ok_or("bar datetime overflow")?
        } else {
            stamp
        };
        let end = start
            .checked_add_signed(duration)
            .ok_or("bar datetime overflow")?;
        require(
            span.phase == Phase::Continuous
                && span.trading_day == day
                && start >= span.start
                && end <= span.end,
            "日内行情交易日错误或跨越休市/集合竞价边界",
        )?;
        Ok(span)
    }
}
pub fn time_id(spec: &TimeSpec) -> Result<String> {
    Ok(Calendar::new(spec.clone())?.id)
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TimeVersion {
    #[schemars(regex(pattern = r"^[a-f0-9]{64}$"))]
    pub id: String,
    pub spec: TimeSpec,
}
impl Validate for TimeVersion {
    fn validate(&self) -> Result<()> {
        require(self.id == time_id(&self.spec)?, "交易时间版本指纹不一致")
    }
}

pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[allow(dead_code)]
    #[serde(untagged)]
    enum Models {
        CalendarDay(CalendarDay),
        Slot(Slot),
        Period(Period),
        ExceptionDay(ExceptionDay),
        TimeSpec(TimeSpec),
        Span(Span),
        TimeVersion(TimeVersion),
    }
    serde_json::to_value(schemars::schema_for!(Models)).expect("JSON schema is serializable")
}

pub fn invoke(operation: &str, input: Value) -> Result<Value> {
    macro_rules! request {($name:ident {$($field:ident:$ty:ty),* $(,)?})=>{{
        #[derive(Deserialize)] #[serde(deny_unknown_fields)] struct $name {$($field:$ty),*}
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
                "CalendarDay" => model!(CalendarDay),
                "Slot" => model!(Slot),
                "Period" => model!(Period),
                "ExceptionDay" => model!(ExceptionDay),
                "TimeSpec" => model!(TimeSpec),
                "Span" => model!(Span),
                "TimeVersion" => model!(TimeVersion),
                _ => Err(format!("unsupported calendar model: {}", r.model)),
            }
        }
        "time_id" => {
            let r = request!(Request { spec: Value });
            output(Calendar::from_value(r.spec)?.id())
        }
        "snapshot" => {
            let r = request!(Request { spec: Value });
            let calendar = Calendar::from_value(r.spec)?;
            output(TimeVersion {
                id: calendar.id,
                spec: calendar.spec,
            })
        }
        "spans" => {
            let r = request!(Request { spec: Value });
            output(Calendar::from_value(r.spec)?.spans())
        }
        "check_contract" => {
            let r = request!(Request {
                spec: Value,
                contract: String
            });
            Calendar::from_value(r.spec)?.check_contract(&r.contract)?;
            Ok(Value::Null)
        }
        "daily" => {
            let r = request!(Request {
                spec: Value,
                contract: String,
                day: NaiveDate
            });
            output(Calendar::from_value(r.spec)?.daily(&r.contract, r.day)?)
        }
        "resolve" => {
            let r = request!(Request {
                spec: Value,
                contract: String,
                stamp: String,
                boundary: Boundary
            });
            let stamp = DateTime::parse_from_rfc3339(&r.stamp).map_err(|e| e.to_string())?;
            output(Calendar::from_value(r.spec)?.resolve(&r.contract, stamp, r.boundary)?)
        }
        "validate_bar" => {
            let r = request!(Request {
                spec: Value,
                contract: String,
                stamp: String,
                day: NaiveDate,
                seconds: i64,
                boundary: BarBoundary
            });
            let stamp = DateTime::parse_from_rfc3339(&r.stamp).map_err(|e| e.to_string())?;
            output(Calendar::from_value(r.spec)?.validate_bar(
                &r.contract,
                stamp,
                r.day,
                r.seconds,
                r.boundary,
            )?)
        }
        _ => Err(format!("unsupported calendar operation: {operation}")),
    }
}
#[cfg(test)]
mod tests;
