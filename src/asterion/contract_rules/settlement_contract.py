"""Frozen settlement evidence and explicit research interpretation."""

from datetime import date
from decimal import Decimal
from typing import Literal

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator


class SettlementRow(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    symbol: str
    exchange: str
    contract: str
    trading_day: date
    settle: Decimal | None
    trading_fee_rate: Decimal | None = Field(ge=0)
    trading_fee: Decimal | None = Field(ge=0)
    delivery_fee: Decimal | None = Field(ge=0)
    b_hedging_margin_rate: Decimal | None = Field(ge=0)
    s_hedging_margin_rate: Decimal | None = Field(ge=0)
    long_margin_rate: Decimal | None = Field(ge=0)
    short_margin_rate: Decimal | None = Field(ge=0)
    offset_today_fee: Decimal | None = Field(ge=0)


from asterion.data.public import Contract, validate_market_code


class SettlementEvidence(BaseModel):
    model_config = ConfigDict(extra="forbid")
    provider: str = Field(min_length=1, max_length=100)
    version_id: str = Field(min_length=1, max_length=100)
    checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    connection_id: str | None = Field(pattern=r"^c_[0-9a-f]{32}$")
    observed_at: AwareDatetime
    row: SettlementRow
    contract: Contract

    @model_validator(mode="after")
    def identity(self):
        validate_market_code(self.row.contract, self.contract)
        if not self.contract.listed_on <= self.row.trading_day <= self.contract.last_trade_on:
            raise ValueError("结算证据超出实际合约生命周期")
        if self.row.exchange != self.contract.product_id.split(".")[0]:
            raise ValueError("结算证据交易所与身份不一致")
        return self


class SettlementBasis(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)
    evidence: SettlementEvidence
    fee_field: Literal["trading_fee", "trading_fee_rate"]
    fee_unit: Literal["yuan_per_lot", "ratio", "percent", "permille", "permyriad"]
    margin_unit: Literal["ratio", "percent"]
    fee_scope: Literal["long_open_and_non_today_close"]
    availability_assumption: Literal["after_source_day"]
    interpretation: str = Field(min_length=1, max_length=1000)

    @model_validator(mode="after")
    def consistent(self):
        if (self.fee_field == "trading_fee") != (self.fee_unit == "yuan_per_lot"):
            raise ValueError("手续费字段与确认单位不一致")
        self.values()
        return self

    def values(self):
        row = self.evidence.row
        fee = getattr(row, self.fee_field)
        margin = row.long_margin_rate
        if fee is None or margin is None:
            raise ValueError("选定手续费或买投机保证金缺失，不能采用该快照")
        divisor = {
            "yuan_per_lot": 1,
            "ratio": 1,
            "percent": 100,
            "permille": 1000,
            "permyriad": 10000,
        }
        fee = fee / divisor[self.fee_unit]
        margin = margin / (100 if self.margin_unit == "percent" else 1)
        if not 0 < margin <= 1 or fee > (10**6 if self.fee_unit == "yuan_per_lot" else 1):
            raise ValueError("单位换算后费用或保证金超出有效范围")
        return ("per_lot" if self.fee_unit == "yuan_per_lot" else "notional", fee, margin)
