"""Minute bars bound to fixed trading-time evidence, from the Rust data store."""

import json

from . import _native


def context(value: dict) -> dict:
    """The validated context; refusals raise ValueError."""
    return json.loads(_native.data_minute_context(json.dumps(value)))


def labels(value: dict, contract: str, trading_day: str) -> list[str]:
    return json.loads(_native.data_minute_labels(json.dumps(value), contract, trading_day))


def window(value: dict, contract: str, trading_day: str) -> dict:
    return json.loads(_native.data_minute_window(json.dumps(value), contract, trading_day))


def bind(value: dict, rows: list[dict], observed: str) -> list[dict]:
    return json.loads(_native.data_minute_bind(json.dumps(value), json.dumps(rows), observed))
