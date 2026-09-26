//! Provider minute bars bound to explicit immutable trading-time evidence: the
//! labels a trading day's sessions allow, the source window of a day, and the
//! binding of normalized rows to those labels and their availability time.
use crate::provider::SourceWindow;
use crate::table::Row;
use asterion_trading_calendar::{BarBoundary, Calendar, TimeVersion};
use chrono::{DateTime, Duration, FixedOffset, NaiveDate, Timelike};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};

pub const TYPE: &str = "futures.minute";
pub const FREQUENCIES: [&str; 5] = ["1m", "5m", "15m", "30m", "60m"];

pub type Result<T> = std::result::Result<T, String>;

/// The fixed basis of a minute task.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MinuteContext {
    pub frequency: String,
    pub trading_time: TimeVersion,
    pub timestamp_semantics: BarBoundary,
    pub semantics_source: String,
}

/// A validated context with its expanded trading-time calendar.
pub struct Minute {
    pub context: MinuteContext,
    calendar: Calendar,
}

fn stamp(text: &str) -> Result<DateTime<FixedOffset>> {
    DateTime::parse_from_rfc3339(text).map_err(|_| "分钟记录时间格式不正确".to_string())
}

impl Minute {
    pub fn new(mut context: MinuteContext) -> Result<Self> {
        if !FREQUENCIES.contains(&context.frequency.as_str()) {
            return Err("分钟周期不受支持".into());
        }
        let length = context.semantics_source.chars().count();
        let source = context.semantics_source.trim().to_string();
        if !(1..=2000).contains(&length) || source.is_empty() {
            return Err("分钟时间含义依据不能为空".into());
        }
        context.semantics_source = source;
        asterion_trading_calendar::Validate::validate(&context.trading_time)?;
        let calendar = Calendar::new(context.trading_time.spec.clone())?;
        Ok(Self { context, calendar })
    }

    pub fn from_value(value: &Value) -> Result<Self> {
        Self::new(
            serde_json::from_value(value.clone()).map_err(|_| "分钟固定依据无效".to_string())?,
        )
    }

    /// Minute labels of a trading day's continuous sessions.
    pub fn label_stamps(
        &self,
        contract: &str,
        day: NaiveDate,
    ) -> Result<Vec<DateTime<FixedOffset>>> {
        let offset = i64::from(self.context.timestamp_semantics == BarBoundary::BarEnd);
        let mut result = Vec::new();
        for span in self.calendar.daily(contract, day)? {
            let seconds = (span.end - span.start).num_seconds();
            if span.start.second() != 0 || span.start.nanosecond() != 0 || seconds % 60 != 0 {
                return Err("分钟采集要求完整分钟交易时段".into());
            }
            result.extend((0..seconds / 60).map(|i| span.start + Duration::minutes(i + offset)));
        }
        result.sort();
        Ok(result)
    }

    /// Expected labels; only verified for one-minute bars.
    pub fn stamps(&self, contract: &str, day: NaiveDate) -> Result<Vec<DateTime<FixedOffset>>> {
        if self.context.frequency != "1m" {
            return Err("供应商多分钟分段规则尚未核验，不能推算预期记录".into());
        }
        self.label_stamps(contract, day)
    }

    /// The source time window covering a trading day's labels.
    pub fn window(&self, contract: &str, day: NaiveDate) -> Result<SourceWindow> {
        let stamps = self.label_stamps(contract, day)?;
        match (stamps.first(), stamps.last()) {
            (Some(start), Some(end)) => Ok(SourceWindow {
                start: *start,
                end: *end,
            }),
            _ => Err("交易日没有可下载的分钟时段".into()),
        }
    }

    /// Check rows sit on allowed labels of their sessions and were observed
    /// after they happened, then record `observed` as their availability time.
    pub fn bind_rows(&self, rows: &mut [Row], observed: &str) -> Result<()> {
        let observed_at = DateTime::parse_from_rfc3339(observed)
            .or_else(|_| DateTime::parse_from_str(observed, "%Y-%m-%dT%H:%M:%S%.f%:z"))
            .map_err(|_| "采集时间格式不正确".to_string())?;
        let mut expected: BTreeMap<(String, NaiveDate), BTreeSet<DateTime<FixedOffset>>> =
            BTreeMap::new();
        for row in rows.iter_mut() {
            let text = |name: &str| row.get(name).and_then(Value::as_str).map(str::to_string);
            if text("frequency").as_deref() != Some(self.context.frequency.as_str()) {
                return Err("分钟记录周期与固定依据不一致".into());
            }
            let contract = text("contract").ok_or("分钟记录缺少合约")?;
            let day = text("trading_day")
                .and_then(|day| NaiveDate::parse_from_str(&day, "%Y-%m-%d").ok())
                .ok_or("分钟记录交易日格式不正确")?;
            let key = (contract.clone(), day);
            if !expected.contains_key(&key) {
                let labels = self.label_stamps(&contract, day)?.into_iter().collect();
                expected.insert(key.clone(), labels);
            }
            let at = stamp(&text("event_time").ok_or("分钟记录缺少时间")?)?;
            if !expected[&key].contains(&at) {
                return Err("分钟记录位于休市、集合竞价或非整分钟边界，未发布".into());
            }
            // Only the session of the label is checked: vendor multi-minute bars
            // may span breaks or be shortened, and no aggregation is invented.
            if self.context.frequency == "1m" {
                self.calendar.validate_bar(
                    &contract,
                    at,
                    day,
                    60,
                    self.context.timestamp_semantics,
                )?;
            }
            if observed_at < at {
                return Err("分钟采集时间早于行情时间".into());
            }
            match row.0.iter_mut().find(|(name, _)| name == "available_at") {
                Some((_, value)) => *value = json!(observed),
                None => row.0.push(("available_at".into(), json!(observed))),
            }
        }
        Ok(())
    }
}
