"""Select a bounded strategy and feed its closed-bar intents to the Rust account."""

from contextlib import closing
from datetime import date
from decimal import Decimal, localcontext
from typing import Literal

from asterion_bindings.execution import ExecutionConfig, ExecutionFactory
from pydantic import Field

from asterion.research.parameters import ParameterValue
from asterion.research.strategies import ClosedBar, StrategyCatalog, StrategyRef

ENGINE = "daily-session-settlement.v6"


class BacktestRequest(ExecutionConfig):
    command_id: str = Field(min_length=1, max_length=100)
    version_id: str = Field(min_length=1, max_length=100)
    strategy: StrategyRef
    parameters: dict[str, ParameterValue]
    capital: Decimal = Decimal(100000)
    slippage_ticks: int = 1
    lots: int = 1
    coverage_report_id: str | None = Field(default=None, min_length=1, max_length=100)
    coverage_policy: Literal["require_complete", "allow_incomplete"] = "require_complete"
    coverage_note: str = Field(default="", max_length=500)
    # Mandatory explicit acknowledgement: neither point-in-time vintages nor calendar completeness.
    assumption: Literal["historical-close-unverified-calendar"]


def frozen_request(value: dict) -> BacktestRequest:
    if not isinstance(value, dict) or set(value) != set(BacktestRequest.model_fields) - {
        "command_id"
    }:
        raise ValueError("研究参数不符合当前固定输入契约")
    request = BacktestRequest.model_validate({"command_id": "compute", **value})
    if request.coverage_policy == "allow_incomplete" and not request.coverage_note.strip():
        raise ValueError("探索模式必须填写原因")
    return request


def calculate(payload: dict, strategies: StrategyCatalog, execution: ExecutionFactory) -> dict:
    if payload["engine"] != ENGINE:
        raise ValueError("不支持此回测引擎版本")
    request = frozen_request(payload["request"])
    # Strategy arithmetic retains the explicit research precision. The account
    # owns its independent fixed precision and cannot observe Python context.
    with localcontext() as ctx:
        ctx.prec = 40
        strategy = strategies.resolve(request.strategy)
        parameters = strategy.parameters(request.parameters)
        warmup = strategy.required_bars(parameters)
        with (
            execution.create(request._native_value()) as account,
            closing(strategy.create(parameters)) as session,
        ):
            for observed, bar in enumerate(payload["bars"], 1):
                closed = account.begin_day(
                    {
                        key: bar.get(key)
                        for key in (
                            "contract",
                            "contract_id",
                            "trading_day",
                            "open",
                            "close",
                            "settle",
                        )
                    }
                )
                intention = session.on_close(
                    ClosedBar(date.fromisoformat(closed["trading_day"]), Decimal(closed["close"]))
                )
                if type(intention) is not bool:
                    raise ValueError("策略必须返回做多或空仓意图")
                account.close_intent(observed >= warmup and intention)
            return account.finish()
