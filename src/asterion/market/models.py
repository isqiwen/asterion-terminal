"""SimNow quote observations, not executable or published contract identities."""

import re
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, SecretStr, field_validator, model_validator


class MarketModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Subscription(MarketModel):
    exchange: Literal["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]
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


class MarketConfiguration(MarketModel):
    version: Literal[1] = 1
    front: str = Field(default="", max_length=200)
    user_id: str = Field(default="", max_length=15, pattern=r"^[A-Za-z0-9]*$")
    subscriptions: list[Subscription] = Field(default_factory=list, max_length=50)

    @field_validator("front")
    @classmethod
    def front_address(cls, value):
        if value:
            match = re.fullmatch(r"tcp://([A-Za-z0-9.-]+):([0-9]{1,5})", value)
            if not match or not 1 <= int(match[2]) <= 65535:
                raise ValueError("行情前置格式应为 tcp://主机:端口")
        return value

    @model_validator(mode="after")
    def unique_symbols(self):
        if len({s.symbol for s in self.subscriptions}) != len(self.subscriptions):
            raise ValueError("自选代码不能重复")
        return self


class MarketConnect(MarketModel):
    password: SecretStr = Field(min_length=1, max_length=40)


class Quote(MarketModel):
    exchange: str
    symbol: str
    last: float | None
    change_percent: float | None
    volume: int | None
    open_interest: float | None
    trading_day: str
    source_time: str
    event_at: float | None
    received_at: float
    stale: bool = True


ConnectionState = Literal["disconnected", "connecting", "connected", "reconnecting", "error"]


class MarketState(MarketModel):
    environment: Literal["simnow"] = "simnow"
    state: ConnectionState
    detail: str
    configuration: MarketConfiguration
    quotes: list[Quote]
    subscription_errors: dict[str, str]
    observed_at: float
