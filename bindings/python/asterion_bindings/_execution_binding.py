"""DTO conversion and owned Rust daily account sessions; no account arithmetic."""

import json
from decimal import Decimal
from typing import ClassVar

from . import _native
from ._models import NativeModel
from .local import current_lifetimes

__all__ = ["ExecutionFactory", "ExecutionModel", "settlement_price", "validate_prices"]


class ExecutionModel(NativeModel):
    _domain: ClassVar[str] = "execution"


def settlement_price(value) -> Decimal:
    return Decimal(json.loads(_native.invoke("execution", "settlement_price", json.dumps(value))))


def validate_prices(prices: dict):
    _native.invoke("execution", "validate", json.dumps({"model": "DailyPrices", "value": prices}))


class DailyAccount:
    def __init__(self, native):
        self._native = native

    def begin_day(self, bar: dict) -> dict:
        return json.loads(self._native.begin_day(json.dumps(bar, ensure_ascii=False)))

    def close_intent(self, long: bool):
        self._native.close_intent(long)

    def finish(self) -> dict:
        return json.loads(self._native.finish())

    def close(self):
        self._native.close()

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()


class ExecutionFactory:
    def __init__(self):
        self._native = _native.ExecutionFactory(list(current_lifetimes()))

    def create(self, config: dict) -> DailyAccount:
        return DailyAccount(
            self._native.create(json.dumps(config, ensure_ascii=False), list(current_lifetimes()))
        )

    def close(self):
        self._native.close()

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()
