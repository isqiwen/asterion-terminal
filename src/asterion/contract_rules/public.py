"""Public immutable rules contract and declared read capability."""

import hashlib
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date, timedelta
from decimal import Decimal
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from asterion.contract_rules.settlement_contract import SettlementBasis
from asterion.data.public import Contract
from asterion.platform.plugins import Capability
from asterion.platform.serialization import canonical
from asterion.trading_time.public import TimeVersion


class Period(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    settlement_basis: SettlementBasis | None
    start: date
    end: date
    margin_rate: Decimal = Field(gt=0, le=1, max_digits=9, decimal_places=8)
    fee_mode: Literal["per_lot", "notional"]
    open_fee: Decimal = Field(ge=0, le=10**6, max_digits=16, decimal_places=8)
    close_fee: Decimal = Field(ge=0, le=10**6, max_digits=16, decimal_places=8)

    @model_validator(mode="after")
    def valid(self):
        if self.end < self.start:
            raise ValueError("规则生效日期范围无效")
        if self.fee_mode == "notional" and max(self.open_fee, self.close_fee) > 1:
            raise ValueError("按成交金额收费的比例须在 0—1 之间")
        if self.settlement_basis:
            basis = self.settlement_basis
            if self.start <= basis.evidence.row.trading_day:
                raise ValueError("盘后结算参数不能在其交易日或更早生效")
            mode, fee, margin = basis.values()
            if (self.fee_mode, self.open_fee, self.close_fee, self.margin_rate) != (
                mode,
                fee,
                fee,
                margin,
            ):
                raise ValueError("规则参数与已确认的结算快照换算不一致")
        return self

    def fee(self, price: Decimal, multiplier: Decimal, lots: int, opening: bool):
        rate = self.open_fee if opening else self.close_fee
        return rate * lots * (price * multiplier if self.fee_mode == "notional" else 1)


class ContractBasis(BaseModel):
    """Frozen source evidence, not a claim of historical rule availability."""

    model_config = ConfigDict(extra="forbid")
    provider: str = Field(min_length=1, max_length=100)
    contract_id: str = Field(
        pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+\.[0-9]{6}\.[0-9]{8}$"
    )
    version_id: str = Field(min_length=1, max_length=100)
    checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    connection_id: str | None = Field(pattern=r"^c_[0-9a-f]{32}$")
    symbol: str = Field(min_length=1, max_length=30)
    exchange: str = Field(min_length=1, max_length=10)
    name: str = Field(min_length=1, max_length=200)
    listed: date
    delisted: date | None
    trade_unit: str | None
    per_unit: str | None
    multiplier: str | None
    quote_unit: str | None
    quote_unit_desc: str | None

    @model_validator(mode="after")
    def exchange_matches(self):
        if self.contract_id.split(".")[0] != self.exchange:
            raise ValueError("标准合约资料的交易所不一致")
        return self


class RuleSpec(BaseModel):
    trading_time: TimeVersion
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False, str_strip_whitespace=True)
    contract: Contract
    title: str = Field(min_length=1, max_length=100)
    source: str = Field(min_length=1, max_length=1000)
    multiplier: Decimal = Field(gt=0, le=10**6, max_digits=16, decimal_places=8)
    tick_size: Decimal = Field(gt=0, le=10**6, max_digits=16, decimal_places=8)
    basis: ContractBasis | None
    periods: list[Period] = Field(min_length=1, max_length=100)

    @model_validator(mode="after")
    def ordered(self):
        time = self.trading_time.spec
        if self.contract.product_id != f"{time.exchange}.{time.product}":
            raise ValueError("规则身份与交易时间品种不一致")
        if self.basis and (
            self.contract.id != self.basis.contract_id
            or self.contract.listed_on != self.basis.listed
            or self.contract.last_trade_on != self.basis.delisted
        ):
            raise ValueError("规则合约与来源资料不一致")
        for period in self.periods:
            if (
                period.settlement_basis
                and period.settlement_basis.evidence.contract.id != self.contract.id
            ):
                raise ValueError("结算参数与规则合约不一致")
        for before, after in zip(self.periods, self.periods[1:]):
            if before.end >= after.start or (after.start - before.end) != timedelta(days=1):
                raise ValueError("规则期间必须按日期排序、连续且不重叠")
        return self

    def at(self, day: date) -> Period:
        for period in self.periods:
            if period.start <= day <= period.end:
                return period
        raise ValueError("规则版本未覆盖所选日期")

    def cover(self, contract_id: str, start: date, end: date):
        if self.contract.id != contract_id:
            raise ValueError("规则版本与行情合约不一致")
        if not self.contract.listed_on <= start <= end <= self.contract.last_trade_on:
            raise ValueError("规则请求超出固定合约生命周期")
        self.at(start)
        self.at(end)


class RuleVersion(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(pattern=r"^[a-f0-9]{64}$")
    spec: RuleSpec

    @model_validator(mode="after")
    def fingerprint(self):
        if self.id != rule_id(self.spec):
            raise ValueError("规则版本指纹不一致")
        return self


def rule_id(spec: RuleSpec):
    return hashlib.sha256(canonical(spec.model_dump(mode="json"))).hexdigest()


@dataclass(frozen=True)
class RuleAccess:
    read: Callable[[str], RuleVersion]


RULE_ACCESS = Capability("contract_rules.read", "asterion.contract_rules", RuleAccess)
