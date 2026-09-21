"""Public source observation and connection contracts, owned by the connections plugin."""

import re
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from threading import Event
from typing import Literal, Protocol

from pydantic import BaseModel, ConfigDict, Field, model_validator

from asterion.platform.plugins import Capability
from asterion.platform.resource import Resource
from asterion.platform.secrets import SecretPort, SecretScope


class SourceModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


Channel = Literal["market", "account"]
Feature = Literal["market_quotes", "instrument_catalog", "account_snapshot", "positions"]
Exchange = Literal["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]


class Subscription(SourceModel):
    exchange: Exchange
    symbol: str = Field(pattern=r"^[A-Za-z]{1,3}[0-9]{3,4}$")

    @model_validator(mode="after")
    def actual_month_code(self):
        match = re.search(r"[0-9]+$", self.symbol)
        if match is None:
            raise ValueError("请输入月份合约代码")
        digits = match.group()
        if not 1 <= int(digits[-2:]) <= 12:
            raise ValueError("请输入月份合约代码，不支持主力、连续或指数代码")
        if len(digits) != (3 if self.exchange == "CZCE" else 4):
            raise ValueError("合约代码位数与交易所不一致")
        return self


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


class QuoteEvent(SourceModel):
    exchange: str
    symbol: str
    last: float | None
    previous_settlement: float | None
    high: float | None
    low: float | None
    volume: int | None
    open_interest: float | None
    trading_day: str
    action_day: str
    source_time: str
    event_at: float | None
    received_at: float


class ReadBatch(SourceModel):
    connection_id: str
    generation: int
    request_id: str
    started_at: float
    observed_at: float
    complete: Literal[True]

    @model_validator(mode="after")
    def times(self):
        if self.observed_at < self.started_at or self.observed_at > time.time() + 5:
            raise ValueError("查询时间无效")
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


class ChannelState(SourceModel):
    state: Literal[
        "disconnected", "connecting", "authenticating", "ready", "reconnecting", "error"
    ] = "disconnected"
    generation: int = 0
    detail: str = "尚未连接"


class ConfigField(SourceModel):
    key: str = Field(pattern=r"^[a-z][a-z0-9_]*$")
    label: str
    secret: bool = False
    required: bool = True
    identity: bool = False
    default: str = ""


class ConnectorDescriptor(SourceModel):
    id: str
    owner: str
    version: Literal[1] = 1
    title: str
    instructions: str = ""
    capabilities: list[Feature]
    fields: list[ConfigField]

    @model_validator(mode="after")
    def unique(self):
        if len({f.key for f in self.fields}) != len(self.fields) or len(
            set(self.capabilities)
        ) != len(self.capabilities):
            raise ValueError("重复接入字段或能力")
        if not self.id or not self.owner:
            raise ValueError("接入贡献缺少身份")
        if any(f.secret and f.default for f in self.fields):
            raise ValueError("秘密字段不能声明默认值")
        return self


class ConnectionProfile(SourceModel):
    connection_id: str = Field(pattern=r"^[a-f0-9]{32}$")
    connector_id: str
    name: str = Field(min_length=1, max_length=60)
    config_revision: int = Field(ge=1)
    config: dict[str, str]


class ConnectorError(ValueError):
    def __init__(self, message, category="unavailable", retryable=True):
        super().__init__(message)
        self.category, self.retryable = category, retryable


class ConnectionSession(Protocol):
    def start_market(self, subscriptions: list[Subscription], emit: Callable) -> None: ...
    def update_subscriptions(self, subscriptions: list[Subscription]) -> None: ...
    def stop_market(self) -> None: ...
    def instruments(self, generation: int, cancel: Event) -> InstrumentBatch: ...
    def account(self, generation: int, cancel: Event) -> AccountBatch: ...
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
