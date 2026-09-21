"""Market views over normalized source observations."""

from datetime import date
from typing import Literal

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

from asterion.connections.public import SourceInstrument, Subscription


class MarketModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Watchlist(MarketModel):
    subscriptions: list[Subscription] = Field(default_factory=list, max_length=50)

    @model_validator(mode="after")
    def unique_symbols(self):
        if len({s.symbol for s in self.subscriptions}) != len(self.subscriptions):
            raise ValueError("自选代码不能重复")
        return self


class Quote(MarketModel):
    exchange: str
    symbol: str
    last: float | None
    previous_settlement: float | None
    change: float | None
    change_percent: float | None
    high: float | None
    low: float | None
    volume: int | None
    open_interest: float | None
    trading_day: str
    source_time: str
    action_day: str
    status: Literal[
        "current", "disconnected", "time_unknown", "time_ahead", "not_updated", "delayed"
    ] = "time_unknown"
    event_at: float | None
    received_at: float
    stale: bool = True


class MarketState(MarketModel):
    connection_id: str
    connection_name: str
    state: str
    detail: str
    configuration: Watchlist
    quotes: list[Quote]
    subscription_errors: dict[str, str]
    contract_names: dict[str, str]
    observed_at: float


ContractDirectoryState = Literal["ready", "missing", "loading", "error", "invalid"]


class ContractChoices(MarketModel):
    exchange: str
    as_of: date
    state: ContractDirectoryState
    detail: str
    source: str
    observed_at: AwareDatetime | None = None
    contracts: list[SourceInstrument] = Field(default_factory=list)
