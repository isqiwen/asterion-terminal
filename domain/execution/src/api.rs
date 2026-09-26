//! The finite daily account contract. Strategy/data selection and persistence
//! belong to its application caller; all account arithmetic belongs here.
use crate::{Result, require};
use asterion_foundation::decimal::Decimal;
use asterion_market_rules::{RuleVersion, Rules, Validate};
use chrono::{Datelike, NaiveDate};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

pub const ENGINE: &str = "daily-session-settlement.v6";
pub const MAX_BARS: usize = 5000;
pub const PRECISION: u32 = 40;

#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ExecutionConfig {
    pub capital: Decimal,
    pub rules: RuleVersion,
    pub start: NaiveDate,
    pub end: NaiveDate,
    #[schemars(range(min = 0, max = 100))]
    pub slippage_ticks: i64,
    #[schemars(range(min = 1, max = 1000))]
    pub lots: i64,
}
impl ExecutionConfig {
    pub fn validate(&self) -> Result<()> {
        self.capital.constraints(true, 1_000_000_000_000, 20, 8)?;
        require((0..=100).contains(&self.slippage_ticks), "滑点数量超出范围")?;
        require((1..=1000).contains(&self.lots), "研究手数超出范围")?;
        require(
            (1..=9999).contains(&self.start.year())
                && (1..=9999).contains(&self.end.year())
                && self.start <= self.end,
            "日期范围不正确",
        )?;
        self.rules.validate()?;
        let rules = Rules::new(self.rules.spec.clone())?;
        rules.cover(&self.rules.spec.contract.id, self.start, self.end)?;
        let periods = &self.rules.spec.trading_time.spec.periods;
        require(
            periods[0].start <= self.start && self.end <= periods[periods.len() - 1].end,
            "交易时间版本未覆盖研究日期范围",
        )
    }
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DailyBar {
    pub contract: String,
    pub contract_id: String,
    pub trading_day: NaiveDate,
    pub open: Decimal,
    pub close: Decimal,
    pub settle: Decimal,
}
impl DailyBar {
    pub fn validate(&self) -> Result<()> {
        for price in [&self.open, &self.close] {
            require(
                price > &Decimal::integer(0) && price <= &Decimal::integer(1_000_000_000_000),
                "日线价格无效",
            )?;
        }
        settlement_price(serde_json::to_value(&self.settle).map_err(|e| e.to_string())?)?;
        Ok(())
    }
}
pub fn settlement_price(value: serde_json::Value) -> Result<Decimal> {
    let result: Result<Decimal> = (|| {
        let value: Decimal = serde_json::from_value(value).map_err(|e| e.to_string())?;
        value.constraints(true, 1_000_000_000_000, 21, 8)?;
        Ok(value)
    })();
    result.map_err(|_| "日线缺少有效结算价，请补齐结算价后选择新数据版本".into())
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ClosedBar {
    pub trading_day: NaiveDate,
    pub close: Decimal,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Fill {
    pub day: NaiveDate,
    pub contract_id: String,
    pub time: String,
    pub side: String,
    pub lots: i64,
    pub price: Decimal,
    pub fee: Decimal,
    pub rule_version: String,
    pub fee_mode: asterion_market_rules::FeeMode,
    pub fee_rate: Decimal,
    pub rule_start: NaiveDate,
    pub reason: String,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct AccountEvent {
    pub day: NaiveDate,
    pub contract_id: String,
    pub reason: String,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CurvePoint {
    pub day: NaiveDate,
    pub contract_id: String,
    pub session_open: String,
    pub session_close: String,
    pub time_version: String,
    pub equity: Decimal,
    pub opening_balance: Decimal,
    pub settle: Decimal,
    pub settlement_pnl: Decimal,
    pub fees: Decimal,
    pub balance: Decimal,
    pub close_pnl: Decimal,
    pub close_equity: Decimal,
    pub drawdown: Decimal,
    pub position: i64,
    pub margin: Decimal,
    pub margin_rate: Decimal,
    pub free_cash: Decimal,
    pub next_target: i64,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Summary {
    pub contract_id: String,
    pub initial_equity: Decimal,
    pub final_equity: Decimal,
    pub net_profit: Decimal,
    pub return_rate: Decimal,
    pub max_drawdown: Decimal,
    pub fees: Decimal,
    pub fill_count: usize,
    pub open_lots: i64,
    pub bars: usize,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ExecutionResult {
    pub summary: Summary,
    pub curve: Vec<CurvePoint>,
    pub fills: Vec<Fill>,
    pub events: Vec<AccountEvent>,
}

/// OHLC admission used for portable daily evidence. Strings are validated but
/// their original spelling remains owned by the immutable package payload.
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DailyPrices {
    pub open: Decimal,
    pub high: Decimal,
    pub low: Decimal,
    pub close: Decimal,
    pub settle: Decimal,
}
impl DailyPrices {
    pub fn validate(&self) -> Result<()> {
        settlement_price(serde_json::to_value(&self.settle).map_err(|e| e.to_string())?)?;
        for value in [&self.open, &self.high, &self.low, &self.close] {
            value
                .constraints(true, 1_000_000_000_000, 21, 8)
                .map_err(|_| "行情价格超出支持范围".to_string())?;
        }
        require(
            self.low <= self.open
                && self.low <= self.close
                && self.open <= self.high
                && self.close <= self.high,
            "行情价格范围无效",
        )
    }
}
