"""Typed views of validated rows. Validation is the Rust data store's; these
models only convert already-accepted values (decimals, dates) for use."""

from datetime import date
from decimal import Decimal

from pydantic import BaseModel

from asterion.data.types import builtin_types


class DailyBar(BaseModel):
    settle: Decimal | None = None
    exchange: str
    symbol: str
    contract: str
    trading_day: date
    open: Decimal
    high: Decimal
    low: Decimal
    close: Decimal
    vol: Decimal
    amount: Decimal | None = None
    oi: Decimal | None = None


def daily_rows(rows: list[dict]) -> list[DailyBar]:
    builtin_types().get("futures.daily").validate(rows)
    return [DailyBar.model_validate(row) for row in rows]
