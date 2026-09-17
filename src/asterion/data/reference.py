"""Versioned reference-data contracts. Values must come from attributed sources."""

from datetime import date
from decimal import Decimal
from itertools import pairwise
from typing import Annotated, Literal

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

Exchange = Literal["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]
Positive = Annotated[Decimal, Field(gt=0, max_digits=24, decimal_places=10)]
Nonnegative = Annotated[Decimal, Field(ge=0, max_digits=24, decimal_places=10)]


class ReferenceModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class Provenance(ReferenceModel):
    source: str = Field(min_length=1)
    source_version: str = Field(min_length=1)
    observed_at: AwareDatetime
    available_at: AwareDatetime

    @model_validator(mode="after")
    def chronology(self):
        if self.available_at < self.observed_at:
            raise ValueError("available_at must not precede observed_at")
        return self


class Product(ReferenceModel):
    id: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+$")
    exchange: Exchange
    name: str = Field(min_length=1)
    currency: str = Field(pattern=r"^[A-Z]{3}$")
    provenance: Provenance

    @model_validator(mode="after")
    def venue(self):
        if not self.id.startswith(self.exchange + "."):
            raise ValueError("Product ID and exchange disagree")
        return self


class Contract(ReferenceModel):
    id: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}$")
    product_id: str
    delivery_month: str = Field(pattern=r"^[0-9]{4}-(0[1-9]|1[0-2])$")
    listed_at: AwareDatetime
    last_trade_at: AwareDatetime
    multiplier: Positive
    tick_size: Positive
    provenance: Provenance

    @model_validator(mode="after")
    def identity_and_time(self):
        if self.id.rstrip("0123456789") != self.product_id:
            raise ValueError("Contract must belong to its product")
        if self.last_trade_at <= self.listed_at:
            raise ValueError("Last trading time must follow listing")
        return self


class Session(ReferenceModel):
    opens_at: AwareDatetime
    closes_at: AwareDatetime
    kind: Literal["day", "night", "auction"]

    @model_validator(mode="after")
    def ordered(self):
        if self.closes_at <= self.opens_at:
            raise ValueError("Session close must follow open")
        return self


class TradingDay(ReferenceModel):
    trading_day: date
    sessions: tuple[Session, ...] = ()
    closed: bool = False

    @model_validator(mode="after")
    def consistent(self):
        if self.closed == bool(self.sessions):
            raise ValueError("Closed dates have no sessions; trading dates require sessions")
        ordered = sorted(self.sessions, key=lambda s: s.opens_at)
        if any(a.closes_at > b.opens_at for a, b in pairwise(ordered)):
            raise ValueError("Trading sessions overlap")
        return self


class SessionCalendar(ReferenceModel):
    product_id: str
    version: str = Field(min_length=1)
    timezone: Literal["Asia/Shanghai"] = "Asia/Shanghai"
    days: tuple[TradingDay, ...]
    provenance: Provenance

    @model_validator(mode="after")
    def unique_dates(self):
        if len({d.trading_day for d in self.days}) != len(self.days):
            raise ValueError("Duplicate trading date")
        sessions = sorted((s for d in self.days for s in d.sessions), key=lambda s: s.opens_at)
        if any(a.closes_at > b.opens_at for a, b in pairwise(sessions)):
            raise ValueError("Sessions of different trading dates overlap")
        return self


class Fee(ReferenceModel):
    per_lot: Nonnegative = Decimal(0)
    notional_rate: Annotated[Decimal, Field(ge=0, lt=1)] = Decimal(0)


class ContractRules(ReferenceModel):
    contract_id: str
    version: str = Field(min_length=1)
    effective_from: AwareDatetime
    effective_until: AwareDatetime | None = None
    margin_rate: Annotated[Decimal, Field(gt=0, le=1)]
    open_fee: Fee
    close_fee: Fee
    close_today_fee: Fee
    requires_close_today: bool
    limit_ratio: Annotated[Decimal, Field(gt=0, lt=1)] | None = None
    provenance: Provenance

    @model_validator(mode="after")
    def interval(self):
        if self.effective_until is not None and self.effective_until <= self.effective_from:
            raise ValueError("Rule interval must be nonempty")
        return self


class ReferenceCatalog(ReferenceModel):
    schema_version: Literal[1] = 1
    products: tuple[Product, ...]
    contracts: tuple[Contract, ...]
    calendars: tuple[SessionCalendar, ...]
    rules: tuple[ContractRules, ...]

    @model_validator(mode="after")
    def relationships(self):
        products = {p.id for p in self.products}
        contracts = {c.id for c in self.contracts}
        if len(products) != len(self.products) or len(contracts) != len(self.contracts):
            raise ValueError("Duplicate product or contract ID")
        if any(c.product_id not in products for c in self.contracts):
            raise ValueError("Unknown contract product")
        if any(c.product_id not in products for c in self.calendars):
            raise ValueError("Unknown calendar product")
        if len({(c.product_id, c.version) for c in self.calendars}) != len(self.calendars):
            raise ValueError("Duplicate calendar version")
        if any(r.contract_id not in contracts for r in self.rules):
            raise ValueError("Unknown rules contract")
        if len({(r.contract_id, r.version) for r in self.rules}) != len(self.rules):
            raise ValueError("Duplicate rules version")
        for contract in contracts:
            rows = sorted(
                (r for r in self.rules if r.contract_id == contract), key=lambda r: r.effective_from
            )
            if any(
                a.effective_until is None or a.effective_until > b.effective_from
                for a, b in pairwise(rows)
            ):
                raise ValueError("Rule effective intervals overlap")
        return self
