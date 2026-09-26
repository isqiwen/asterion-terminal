//! One owner for the current single-contract, daily long/flat account.
//! This does not implement broker execution, short positions or order recovery.
mod api;
pub use api::*;
use asterion_foundation::decimal::Decimal;
use asterion_market_rules::Rules;
use asterion_trading_calendar::Calendar;
use chrono::{DateTime, FixedOffset, Timelike};
use schemars::JsonSchema;
use serde_json::Value;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

pub type Result<T> = std::result::Result<T, String>;
fn require(ok: bool, message: &str) -> Result<()> {
    if ok { Ok(()) } else { Err(message.into()) }
}
fn add(a: &Decimal, b: &Decimal) -> Result<Decimal> {
    a.add(b, PRECISION)
}
fn sub(a: &Decimal, b: &Decimal) -> Result<Decimal> {
    a.subtract(b, PRECISION)
}
fn mul(a: &Decimal, b: &Decimal) -> Result<Decimal> {
    a.multiply(b, PRECISION)
}
fn div(a: &Decimal, b: &Decimal) -> Result<Decimal> {
    a.divide(b, PRECISION)
}
fn integer(value: i64) -> Decimal {
    Decimal::integer(value)
}
fn first_max(a: &Decimal, b: &Decimal) -> Decimal {
    if b > a { b.clone() } else { a.clone() }
}
fn timestamp(value: DateTime<FixedOffset>) -> String {
    let mut result = value.format("%Y-%m-%dT%H:%M:%S").to_string();
    if value.nanosecond() != 0 {
        result.push_str(&format!(".{:06}", value.nanosecond() / 1000));
    }
    result.push_str(&value.format("%:z").to_string());
    result
}
#[derive(Clone)]
struct Account {
    equity: Decimal,
    peak: Decimal,
    position: i64,
    target: i64,
    previous_settle: Option<Decimal>,
    total_fees: Decimal,
    margin_call: bool,
    max_drawdown: Decimal,
}
struct Pending {
    account: Account,
    point: CurvePoint,
    fill: Option<Fill>,
    events: Vec<AccountEvent>,
}
/// Owns the lifetime of every account created by this domain resource.
pub struct ExecutionFactory {
    active: Arc<AtomicBool>,
}
impl Default for ExecutionFactory {
    fn default() -> Self {
        Self {
            active: Arc::new(AtomicBool::new(true)),
        }
    }
}
impl ExecutionFactory {
    pub fn open(&self, config: ExecutionConfig) -> Result<DailyAccount> {
        require(self.active.load(Ordering::Acquire), "执行资源已关闭")?;
        DailyAccount::new(config, self.active.clone())
    }
    pub fn close(&self) {
        self.active.store(false, Ordering::Release);
    }
}
impl Drop for ExecutionFactory {
    fn drop(&mut self) {
        self.close();
    }
}
pub struct DailyAccount {
    owner: Arc<AtomicBool>,
    config: ExecutionConfig,
    rules: Rules,
    calendar: Calendar,
    slippage: Decimal,
    account: Account,
    pending: Option<Pending>,
    curve: Vec<CurvePoint>,
    fills: Vec<Fill>,
    events: Vec<AccountEvent>,
    closed: bool,
}
impl DailyAccount {
    pub fn check(&self) -> Result<()> {
        require(
            !self.closed && self.owner.load(Ordering::Acquire),
            "账户计算已关闭",
        )
    }
    fn new(config: ExecutionConfig, owner: Arc<AtomicBool>) -> Result<Self> {
        config.validate()?;
        let rules = Rules::new(config.rules.spec.clone())?;
        let calendar = Calendar::new(config.rules.spec.trading_time.spec.clone())?;
        let slippage = mul(
            &config.rules.spec.tick_size,
            &integer(config.slippage_ticks),
        )?;
        let account = Account {
            equity: config.capital.clone(),
            peak: config.capital.clone(),
            position: 0,
            target: 0,
            previous_settle: None,
            total_fees: integer(0),
            margin_call: false,
            max_drawdown: integer(0),
        };
        Ok(Self {
            owner,
            config,
            rules,
            calendar,
            slippage,
            account,
            pending: None,
            curve: Vec::new(),
            fills: Vec::new(),
            events: Vec::new(),
            closed: false,
        })
    }
    fn prepare(&self, bar: &DailyBar) -> Result<Pending> {
        self.check()?;
        require(self.pending.is_none(), "上一根日线尚未提供收盘意图")?;
        require(self.curve.len() < MAX_BARS, "研究超过5000根日线上限")?;
        bar.validate()?;
        require(
            bar.contract_id == self.config.rules.spec.contract.id,
            "执行输入必须属于单个实际合约生命周期",
        )?;
        require(
            self.config.start <= bar.trading_day && bar.trading_day <= self.config.end,
            "日线超出研究日期范围",
        )?;
        require(
            self.curve
                .last()
                .is_none_or(|last| last.day < bar.trading_day),
            "日线必须按交易日递增且不重复",
        )?;
        let rule = self.rules.at(bar.trading_day)?;
        let sessions = self.calendar.daily(&bar.contract, bar.trading_day)?;
        let mut a = self.account.clone();
        let opening_balance = a.equity.clone();
        let mut day_fees = integer(0);
        let multiplier = &self.config.rules.spec.multiplier;
        if let Some(previous) = &a.previous_settle {
            a.equity = add(
                &a.equity,
                &mul(
                    &mul(&sub(&bar.open, previous)?, &integer(a.position))?,
                    multiplier,
                )?,
            )?;
        }
        let gap_margin = mul(
            &mul(&mul(&bar.open, &integer(a.position))?, multiplier)?,
            &rule.margin_rate,
        )?;
        let forced = a.margin_call || (a.position > 0 && a.equity < gap_margin);
        let desired = if forced || a.equity <= integer(0) {
            0
        } else {
            a.target
        };
        let delta = desired - a.position;
        let mut fill = None;
        let mut events = Vec::new();
        if delta != 0 {
            let price = if delta > 0 {
                add(&bar.open, &self.slippage)?
            } else {
                sub(&bar.open, &self.slippage)?
            };
            require(price > integer(0), "滑点导致非正成交价，请检查最小变动价位")?;
            let fee = self
                .rules
                .fee(bar.trading_day, &price, delta.abs(), delta > 0, PRECISION)?;
            let cost = add(
                &mul(&mul(&integer(delta.abs()), &self.slippage)?, multiplier)?,
                &fee,
            )?;
            let required = mul(
                &mul(
                    &mul(&first_max(&bar.open, &price), &integer(desired))?,
                    multiplier,
                )?,
                &rule.margin_rate,
            )?;
            if delta > 0 && sub(&a.equity, &cost)? < required {
                events.push(AccountEvent {
                    day: bar.trading_day,
                    contract_id: bar.contract_id.clone(),
                    reason: "资金不足，拒绝开仓".into(),
                });
            } else {
                a.equity = sub(&a.equity, &cost)?;
                a.total_fees = add(&a.total_fees, &fee)?;
                day_fees = add(&day_fees, &fee)?;
                a.position = desired;
                fill = Some(Fill {
                    day: bar.trading_day,
                    contract_id: bar.contract_id.clone(),
                    time: timestamp(sessions[0].start),
                    side: if delta > 0 { "BUY" } else { "SELL" }.into(),
                    lots: delta.abs(),
                    price,
                    fee,
                    rule_version: self.config.rules.id.clone(),
                    fee_mode: rule.fee_mode,
                    fee_rate: if delta > 0 {
                        &rule.open_fee
                    } else {
                        &rule.close_fee
                    }
                    .clone(),
                    rule_start: rule.start,
                    reason: if forced {
                        "保证金不足平仓"
                    } else {
                        "上一根日线信号"
                    }
                    .into(),
                });
            }
        }
        a.equity = add(
            &a.equity,
            &mul(
                &mul(&sub(&bar.settle, &bar.open)?, &integer(a.position))?,
                multiplier,
            )?,
        )?;
        let settlement_pnl = add(&sub(&a.equity, &opening_balance)?, &day_fees)?;
        let close_pnl = mul(
            &mul(&sub(&bar.close, &bar.settle)?, &integer(a.position))?,
            multiplier,
        )?;
        let margin = mul(
            &mul(&mul(&bar.settle, &integer(a.position))?, multiplier)?,
            &rule.margin_rate,
        )?;
        a.margin_call = a.position > 0 && a.equity < margin;
        if a.margin_call {
            events.push(AccountEvent {
                day: bar.trading_day,
                contract_id: bar.contract_id.clone(),
                reason: "保证金不足，下一根日线开盘尝试平仓".into(),
            });
        }
        a.peak = first_max(&a.peak, &a.equity);
        let drawdown = div(&sub(&a.peak, &a.equity)?, &a.peak)?;
        a.max_drawdown = first_max(&a.max_drawdown, &drawdown);
        let point = CurvePoint {
            day: bar.trading_day,
            contract_id: bar.contract_id.clone(),
            session_open: timestamp(sessions[0].start),
            session_close: timestamp(sessions[sessions.len() - 1].end),
            time_version: self.config.rules.spec.trading_time.id.clone(),
            equity: a.equity.clone(),
            opening_balance,
            settle: bar.settle.clone(),
            settlement_pnl,
            fees: day_fees,
            balance: a.equity.clone(),
            close_equity: add(&a.equity, &close_pnl)?,
            close_pnl,
            drawdown,
            position: a.position,
            free_cash: sub(&a.equity, &margin)?,
            margin,
            margin_rate: rule.margin_rate.clone(),
            next_target: 0,
        };
        a.previous_settle = Some(bar.settle.clone());
        Ok(Pending {
            account: a,
            point,
            fill,
            events,
        })
    }
    pub fn begin_day(&mut self, bar: DailyBar) -> Result<ClosedBar> {
        match self.prepare(&bar) {
            Ok(pending) => {
                self.pending = Some(pending);
                Ok(ClosedBar {
                    trading_day: bar.trading_day,
                    close: bar.close,
                })
            }
            Err(error) => {
                self.closed = true;
                Err(error)
            }
        }
    }
    pub fn close_intent(&mut self, long: bool) -> Result<()> {
        self.check()?;
        let Some(mut pending) = self.pending.take() else {
            self.closed = true;
            return Err("尚无等待收盘意图的日线".into());
        };
        pending.account.target = if long { self.config.lots } else { 0 };
        pending.point.next_target = pending.account.target;
        self.account = pending.account;
        self.curve.push(pending.point);
        self.fills.extend(pending.fill);
        self.events.extend(pending.events);
        Ok(())
    }
    pub fn finish(self) -> Result<ExecutionResult> {
        self.check()?;
        require(
            self.pending.is_none() && !self.curve.is_empty(),
            "账户尚无完整日线结果",
        )?;
        let a = self.account;
        Ok(ExecutionResult {
            summary: Summary {
                contract_id: self.config.rules.spec.contract.id,
                initial_equity: self.config.capital.clone(),
                net_profit: sub(&a.equity, &self.config.capital)?,
                return_rate: sub(&div(&a.equity, &self.config.capital)?, &integer(1))?,
                final_equity: a.equity,
                max_drawdown: a.max_drawdown,
                fees: a.total_fees,
                fill_count: self.fills.len(),
                open_lots: a.position,
                bars: self.curve.len(),
            },
            curve: self.curve,
            fills: self.fills,
            events: self.events,
        })
    }
}
pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[allow(dead_code)]
    #[serde(untagged)]
    enum Models {
        Config(Box<ExecutionConfig>),
        Bar(DailyBar),
        Prices(DailyPrices),
        Closed(ClosedBar),
        Output(Box<ExecutionResult>),
    }
    serde_json::to_value(schemars::schema_for!(Models)).expect("schema serializes")
}
pub fn invoke(operation: &str, value: Value) -> Result<Value> {
    match operation {
        "schema" => Ok(schema()),
        "settlement_price" => {
            serde_json::to_value(settlement_price(value)?).map_err(|e| e.to_string())
        }
        "validate" => {
            let name = value
                .get("model")
                .and_then(Value::as_str)
                .ok_or("missing execution model")?;
            let model = value.get("value").ok_or("missing execution value")?.clone();
            match name {
                "ExecutionConfig" => serde_json::from_value::<ExecutionConfig>(model)
                    .map_err(|e| e.to_string())?
                    .validate()?,
                "DailyPrices" => serde_json::from_value::<DailyPrices>(model)
                    .map_err(|e| e.to_string())?
                    .validate()?,
                "DailyBar" => serde_json::from_value::<DailyBar>(model)
                    .map_err(|e| e.to_string())?
                    .validate()?,
                "ExecutionResult" => {
                    let _: ExecutionResult =
                        serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                "Summary" => {
                    let _: Summary = serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                "CurvePoint" => {
                    let _: CurvePoint = serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                "Fill" => {
                    let _: Fill = serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                "AccountEvent" => {
                    let _: AccountEvent =
                        serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                "ClosedBar" => {
                    let _: ClosedBar = serde_json::from_value(model).map_err(|e| e.to_string())?;
                }
                _ => return Err("unsupported execution model".into()),
            }
            Ok(Value::Null)
        }
        _ => Err("unsupported execution operation".into()),
    }
}
