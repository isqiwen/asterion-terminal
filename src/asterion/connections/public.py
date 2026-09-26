"""Public source observation and connection contracts, owned by the connections plugin."""

from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from threading import Event
from typing import Literal, Protocol

from asterion_bindings.connections import (
    Channel,
    ChannelState,
    ConfigField,
    ConnectionProfile,
    ConnectorDescriptor,
    Exchange,
    Feature,
    ReadBatch,
    ReadRequest,
    Subscription,
)
from asterion_bindings.plugin_host import Capability
from asterion_bindings.resource import Resource
from asterion_bindings.secrets import SecretPort, SecretScope
from pydantic import BaseModel, ConfigDict, Field, model_validator

__all__ = [
    "CONNECTIONS",
    "CONNECTOR_OWNERS",
    "CREDENTIALS",
    "CREDENTIAL_SCOPE",
    "AccountBatch",
    "AccountSummary",
    "Channel",
    "ChannelState",
    "ConfigField",
    "ConnectionAccess",
    "ConnectionProfile",
    "ConnectionSession",
    "ConnectorContribution",
    "ConnectorDescriptor",
    "ConnectorError",
    "Exchange",
    "Feature",
    "InstrumentBatch",
    "Position",
    "ReadBatch",
    "ReadRequest",
    "SourceInstrument",
    "SourceModel",
    "Subscription",
]


class SourceModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class SourceInstrument(Subscription):
    name: str
    product: str
    delivery_month: str = Field(pattern=r"^[0-9]{4}-(0[1-9]|1[0-2])$")
    listed_on: date
    last_trade_on: date

    @model_validator(mode="after")
    def lifecycle(self):
        digits = self.delivery_month.replace("-", "")[-(3 if self.exchange == "CZCE" else 4) :]
        product = (
            self.product.upper() if self.exchange in {"CZCE", "CFFEX"} else self.product.lower()
        )
        if self.last_trade_on < self.listed_on or self.symbol != product + digits:
            raise ValueError("合约日期或代码与完整交割月份不匹配")
        return self


class AccountSummary(SourceModel):
    currency: Literal["CNY"] = "CNY"
    trading_day: str
    balance: Decimal | None
    available: Decimal | None
    margin: Decimal | None
    position_profit: Decimal | None
    close_profit: Decimal | None
    commission: Decimal | None


class Position(SourceModel):
    exchange: str
    symbol: str
    name: str | None = None
    direction: Literal["long", "short"]
    hedge: str
    quantity: int = Field(ge=0, strict=True)
    today: int = Field(ge=0, strict=True)
    yesterday: int = Field(ge=0, strict=True)
    margin: Decimal | None
    profit: Decimal | None

    @model_validator(mode="after")
    def quantities(self):
        if self.today + self.yesterday != self.quantity:
            raise ValueError("今昨持仓与总量不一致")
        return self


class InstrumentBatch(ReadBatch):
    instruments: list[SourceInstrument] = Field(max_length=10000)

    @model_validator(mode="after")
    def unique(self):
        if len({(r.exchange, r.symbol) for r in self.instruments}) != len(self.instruments):
            raise ValueError("来源合约重复")
        return self


class AccountBatch(ReadBatch):
    source_account_id: str
    account: AccountSummary
    positions: list[Position] = Field(max_length=10000)

    @model_validator(mode="after")
    def unique(self):
        if not self.source_account_id or len(
            {(r.exchange, r.symbol, r.direction, r.hedge) for r in self.positions}
        ) != len(self.positions):
            raise ValueError("账户身份缺失或持仓重复")
        return self


class ConnectorError(ValueError):
    def __init__(self, message, category="unavailable", retryable=True):
        super().__init__(message)
        self.category, self.retryable = category, retryable


class ConnectionSession(Protocol):
    def start_market(self, subscriptions: list[Subscription], emit: Callable) -> None: ...
    def update_subscriptions(self, subscriptions: list[Subscription]) -> None: ...
    def stop_market(self) -> None: ...
    def instruments(self, request: ReadRequest, cancel: Event) -> InstrumentBatch: ...
    def account(self, request: ReadRequest, cancel: Event) -> AccountBatch: ...
    def close(self) -> None: ...


@dataclass(frozen=True)
class ConnectorContribution:
    descriptor: ConnectorDescriptor
    validate: Callable[[dict[str, str], dict[str, str]], None]
    factory: Callable[[ConnectionProfile, dict[str, str]], ConnectionSession]


@dataclass(frozen=True)
class ConnectionAccess:
    profiles: Callable[[], list[str]]
    profile: Callable[[str], ConnectionProfile]
    channel: Callable[[str, Channel], ChannelState]
    subscribe: Callable[[str, list[Subscription]], None]
    instruments: Callable[[str, Event], InstrumentBatch]
    account: Callable[[str, Event], AccountBatch]
    listen: Callable[[Callable[[str, int, str, object], None]], Callable[[], None]]
    supports: Callable[[str, Feature], bool]


CONNECTIONS = Capability("connections.access", "asterion.connections", ConnectionAccess)
CREDENTIAL_SCOPE = SecretScope(b"asterion.connections.credentials.v1", b"connection-profile-v1")
CREDENTIALS = Resource("connections.credentials", SecretPort)
CONNECTOR_OWNERS = Resource("connections.connector_owners", dict)
