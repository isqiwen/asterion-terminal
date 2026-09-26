"""Market views over normalized source observations."""

from datetime import date
from typing import Literal

from asterion_bindings.market_feed import Quote
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
