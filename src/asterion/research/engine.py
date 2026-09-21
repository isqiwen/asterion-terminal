"""Deterministic daily long/flat execution model. Strategy execution is supplied through a bounded session."""

from contextlib import closing
from datetime import date
from decimal import Decimal, localcontext
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from asterion.contract_rules.public import RuleVersion
from asterion.research.parameters import ParameterValue
from asterion.research.strategies import ClosedBar, StrategyCatalog, StrategyRef

ENGINE = "daily-session-settlement.v6"


class BacktestRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    command_id: str = Field(min_length=1, max_length=100)
    version_id: str = Field(min_length=1, max_length=100)
    start: date
    end: date
    strategy: StrategyRef
    parameters: dict[str, ParameterValue]
    capital: Decimal = Field(
        default=Decimal(100000), gt=0, le=10**12, max_digits=20, decimal_places=8
    )
    rules: RuleVersion
    slippage_ticks: int = Field(default=1, ge=0, le=100)
    lots: int = Field(default=1, ge=1, le=1000)
    coverage_report_id: str | None = Field(default=None, min_length=1, max_length=100)
    coverage_policy: Literal["require_complete", "allow_incomplete"] = "require_complete"
    coverage_note: str = Field(default="", max_length=500)
    # Mandatory explicit acknowledgement: neither point-in-time vintages nor calendar completeness.
    assumption: Literal["historical-close-unverified-calendar"]

    @model_validator(mode="after")
    def valid_range(self):
        if self.end < self.start:
            raise ValueError("日期范围不正确")
        self.rules.spec.at(self.start)
        self.rules.spec.at(self.end)
        timing = self.rules.spec.trading_time.spec
        if self.start < timing.periods[0].start or self.end > timing.periods[-1].end:
            raise ValueError("交易时间版本未覆盖研究日期范围")
        return self


def frozen_request(value: dict) -> BacktestRequest:
    if not isinstance(value, dict) or set(value) != set(BacktestRequest.model_fields) - {
        "command_id"
    }:
        raise ValueError("研究参数不符合当前固定输入契约")
    request = BacktestRequest.model_validate({"command_id": "compute", **value})
    if request.coverage_policy == "allow_incomplete" and not request.coverage_note.strip():
        raise ValueError("探索模式必须填写原因")
    return request


def settlement_price(bar: dict) -> Decimal:
    try:
        value = Decimal(str(bar["settle"]))
        if not value.is_finite() or not 0 < value <= Decimal("1e12"):
            raise ValueError
        if int(value.normalize().as_tuple().exponent) < -8:
            raise ValueError
        return value
    except (KeyError, ValueError, ArithmeticError):
        raise ValueError("日线缺少有效结算价，请补齐结算价后选择新数据版本") from None


def calculate(payload: dict, strategies: StrategyCatalog) -> dict:
    if payload["engine"] != ENGINE:
        raise ValueError("不支持此回测引擎版本")
    request = frozen_request(payload["request"])
    with localcontext() as ctx:
        ctx.prec = 40
        strategy = strategies.resolve(request.strategy)
        parameters = strategy.parameters(request.parameters)
        warmup = strategy.required_bars(parameters)
        with closing(strategy.create(parameters)) as session:
            return _calculate(request, payload["bars"], session, warmup)


def _calculate(config: BacktestRequest, bars: list[dict], session, warmup: int) -> dict:
    if not bars or len({bar["contract_id"] for bar in bars}) != 1:
        raise ValueError("执行输入必须属于单个实际合约生命周期")
    equity = peak = config.capital
    position = target = 0
    previous_settle = None
    fills, curve, events = [], [], []
    total_fees = Decimal(0)
    slippage = config.rules.spec.tick_size * config.slippage_ticks
    margin_call = False
    max_drawdown = Decimal(0)
    for observed, bar in enumerate(bars, 1):
        day = bar["trading_day"]
        config.rules.spec.cover(bar["contract_id"], config.start, config.end)
        rule = config.rules.spec.at(date.fromisoformat(day))
        sessions = config.rules.spec.trading_time.spec.daily(
            bar["contract"], date.fromisoformat(day)
        )
        opening, close = Decimal(bar["open"]), Decimal(bar["close"])
        settle = settlement_price(bar)
        opening_balance = equity
        day_fees = Decimal(0)
        # Mark the existing position across the overnight gap before changing it.
        if previous_settle is not None:
            equity += (opening - previous_settle) * position * config.rules.spec.multiplier
        gap_margin = opening * position * config.rules.spec.multiplier * rule.margin_rate
        forced = margin_call or (position > 0 and equity < gap_margin)
        desired = 0 if forced or equity <= 0 else target
        delta = desired - position
        if delta:
            price = opening + (slippage if delta > 0 else -slippage)
            if price <= 0:
                raise ValueError("滑点导致非正成交价，请检查最小变动价位")
            fee = rule.fee(price, config.rules.spec.multiplier, abs(delta), delta > 0)
            cost = abs(delta) * slippage * config.rules.spec.multiplier + fee
            required = (
                max(opening, price) * desired * config.rules.spec.multiplier * rule.margin_rate
            )
            if delta > 0 and equity - cost < required:
                events.append(
                    {"day": day, "contract_id": bar["contract_id"], "reason": "资金不足，拒绝开仓"}
                )
            else:
                equity -= cost
                total_fees += fee
                day_fees += fee
                position = desired
                fills.append(
                    {
                        "day": day,
                        "contract_id": bar["contract_id"],
                        "time": sessions[0].start.isoformat(),
                        "side": "BUY" if delta > 0 else "SELL",
                        "lots": abs(delta),
                        "price": str(price),
                        "fee": str(fee),
                        "rule_version": config.rules.id,
                        "fee_mode": rule.fee_mode,
                        "fee_rate": str(rule.open_fee if delta > 0 else rule.close_fee),
                        "rule_start": rule.start.isoformat(),
                        "reason": "保证金不足平仓" if forced else "上一根日线信号",
                    }
                )
        equity += (settle - opening) * position * config.rules.spec.multiplier
        settlement_pnl = equity - opening_balance + day_fees
        close_pnl = (close - settle) * position * config.rules.spec.multiplier
        margin = settle * position * config.rules.spec.multiplier * rule.margin_rate
        margin_call = position > 0 and equity < margin
        if margin_call:
            events.append(
                {
                    "day": day,
                    "contract_id": bar["contract_id"],
                    "reason": "保证金不足，下一根日线开盘尝试平仓",
                }
            )
        peak = max(peak, equity)
        drawdown = (peak - equity) / peak
        max_drawdown = max(max_drawdown, drawdown)
        intention = session.on_close(ClosedBar(date.fromisoformat(day), close))
        if type(intention) is not bool:
            raise ValueError("策略必须返回做多或空仓意图")
        target = config.lots if observed >= warmup and intention else 0
        curve.append(
            {
                "day": day,
                "contract_id": bar["contract_id"],
                "session_open": sessions[0].start.isoformat(),
                "session_close": sessions[-1].end.isoformat(),
                "time_version": config.rules.spec.trading_time.id,
                "equity": str(equity),
                "opening_balance": str(opening_balance),
                "settle": str(settle),
                "settlement_pnl": str(settlement_pnl),
                "fees": str(day_fees),
                "balance": str(equity),
                "close_pnl": str(close_pnl),
                "close_equity": str(equity + close_pnl),
                "drawdown": str(drawdown),
                "position": position,
                "margin": str(margin),
                "margin_rate": str(rule.margin_rate),
                "free_cash": str(equity - margin),
                "next_target": target,
            }
        )
        previous_settle = settle
    return {
        "summary": {
            "contract_id": bars[0]["contract_id"],
            "initial_equity": str(config.capital),
            "final_equity": str(equity),
            "net_profit": str(equity - config.capital),
            "return_rate": str(equity / config.capital - 1),
            "max_drawdown": str(max_drawdown),
            "fees": str(total_fees),
            "fill_count": len(fills),
            "open_lots": position,
            "bars": len(bars),
        },
        "curve": curve,
        "fills": fills,
        "events": events,
    }
