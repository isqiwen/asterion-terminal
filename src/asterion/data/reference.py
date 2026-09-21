"""Versioned reference-data contracts. Values must come from attributed sources."""

import re
from datetime import date
from itertools import pairwise
from typing import Literal

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

Exchange = Literal["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]


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
    id: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+$")
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
    id: str = Field(min_length=1, max_length=100)
    product_id: str
    delivery_month: str = Field(pattern=r"^[0-9]{4}-(0[1-9]|1[0-2])$")
    listed_on: date
    last_trade_on: date
    last_delivery_on: date | None
    provenance: Provenance

    @model_validator(mode="after")
    def identity_and_time(self):
        date.fromisoformat(self.delivery_month + "-01")
        expected = (
            f"{self.product_id}.{self.delivery_month.replace('-', '')}.{self.listed_on:%Y%m%d}"
        )
        if self.id != expected:
            raise ValueError("实际合约身份必须匹配品种、完整交割年月与上市日期")
        if self.last_trade_on < self.listed_on:
            raise ValueError("最后交易日不得早于上市日")
        if self.last_delivery_on is not None and self.last_delivery_on < self.last_trade_on:
            raise ValueError("最后交割日不得早于最后交易日")
        return self


class SourceSymbol(ReferenceModel):
    source: str = Field(pattern=r"^[A-Za-z][A-Za-z0-9_.:-]{0,63}$")
    symbol: str = Field(min_length=1, max_length=64, pattern=r"^\S+$")
    contract_id: str = Field(min_length=1, max_length=100)
    valid_from: date
    valid_until: date
    provenance: Provenance

    @model_validator(mode="after")
    def interval(self):
        if self.valid_until < self.valid_from:
            raise ValueError("来源代码有效日期范围无效")
        return self


class ResolutionRequest(ReferenceModel):
    source: str = Field(min_length=1, max_length=64)
    symbol: str = Field(min_length=1, max_length=64)
    trading_day: date
    information_at: AwareDatetime


class ContractResolution(ReferenceModel):
    contract: Contract
    mapping: SourceSymbol


class CatalogInput(ReferenceModel):
    version_id: str = Field(min_length=1, max_length=100)
    checksum: str = Field(pattern=r"^[0-9a-f]{64}$")
    source: str = Field(min_length=1, max_length=64)


class ReferenceCatalog(ReferenceModel):
    schema_version: Literal[2]
    inputs: tuple[CatalogInput, ...]
    products: tuple[Product, ...]
    contracts: tuple[Contract, ...]
    symbols: tuple[SourceSymbol, ...]

    @model_validator(mode="after")
    def relationships(self):
        if len({item.version_id for item in self.inputs}) != len(self.inputs):
            raise ValueError("目录输入版本重复")
        products = {p.id for p in self.products}
        contracts = {c.id for c in self.contracts}
        if len(products) != len(self.products) or len(contracts) != len(self.contracts):
            raise ValueError("Duplicate product or contract ID")
        if any(c.product_id not in products for c in self.contracts):
            raise ValueError("Unknown contract product")
        lifecycles: dict[tuple[str, str], list[Contract]] = {}
        for contract in self.contracts:
            lifecycles.setdefault((contract.product_id, contract.delivery_month), []).append(
                contract
            )
        for group in lifecycles.values():
            ordered_contracts = sorted(group, key=lambda c: c.listed_on)
            if any(a.last_trade_on >= b.listed_on for a, b in pairwise(ordered_contracts)):
                raise ValueError("同一品种交割月份存在重叠合约生命周期")
        by_id = {c.id: c for c in self.contracts}
        groups: dict[tuple[str, str], list[SourceSymbol]] = {}
        for mapping in self.symbols:
            contract = by_id.get(mapping.contract_id)
            if contract is None:
                raise ValueError("来源代码引用未知实际合约")
            if not (
                contract.listed_on
                <= mapping.valid_from
                <= mapping.valid_until
                <= contract.last_trade_on
            ):
                raise ValueError("来源代码有效期超出实际合约上市范围")
            groups.setdefault((mapping.source, mapping.symbol), []).append(mapping)
        for group in groups.values():
            ordered = sorted(group, key=lambda m: m.valid_from)
            if any(a.valid_until >= b.valid_from for a, b in pairwise(ordered)):
                raise ValueError("同一来源代码存在重叠映射")
        return self

    def resolve(self, request: ResolutionRequest) -> ContractResolution:
        matches = [
            m
            for m in self.symbols
            if m.source == request.source
            and m.symbol == request.symbol
            and m.valid_from <= request.trading_day <= m.valid_until
        ]
        if len(matches) != 1:
            raise ValueError("没有唯一适用的来源代码映射")
        mapping = matches[0]
        contract = next(c for c in self.contracts if c.id == mapping.contract_id)
        product = next(p for p in self.products if p.id == contract.product_id)
        if any(
            item.provenance.available_at > request.information_at
            for item in (mapping, contract, product)
        ):
            raise ValueError("身份或映射依据在指定信息截止时间尚不可知")
        return ContractResolution(contract=contract, mapping=mapping)


class SourceIdentity(ReferenceModel):
    catalog_id: str
    catalog: ReferenceCatalog
    source: str
    symbol: str
    information_at: AwareDatetime

    @model_validator(mode="after")
    def fingerprint(self):
        from asterion.data.reference_store import catalog_digest

        if self.catalog_id != catalog_digest(self.catalog):
            raise ValueError("来源身份目录指纹不一致")
        if len(self.catalog.inputs) != 1 or self.catalog.inputs[0].source != self.source:
            raise ValueError("来源身份必须绑定唯一同源资料版本")
        if not any(
            item.source == self.source and item.symbol == self.symbol
            for item in self.catalog.symbols
        ):
            raise ValueError("来源身份代码不在目录中")
        return self

    def resolve(self, day: date):
        return self.catalog.resolve(
            ResolutionRequest(
                source=self.source,
                symbol=self.symbol,
                trading_day=day,
                information_at=self.information_at,
            )
        ).contract

    def validate_rows(self, rows: list[dict], exchange: str) -> list[str]:
        identifiers = set()
        for row in rows:
            actual = self.resolve(date.fromisoformat(row["trading_day"]))
            if (
                row["symbol"] != self.symbol
                or row["exchange"] != exchange
                or not actual.product_id.startswith(exchange + ".")
            ):
                raise ValueError("行情记录与固定来源合约不一致")
            validate_market_code(row["contract"], actual)
            identifiers.add(actual.id)
        if len(identifiers) != 1:
            raise ValueError("同步数据必须属于单一实际合约生命周期")
        return sorted(identifiers)


def validate_market_code(contract: str, result: Contract):
    product = re.fullmatch(r"([A-Z]+)\.([A-Za-z]+)([0-9]{3,4})", contract)
    if product is None or f"{product[1]}.{product[2].upper()}" != result.product_id:
        raise ValueError("文件代码与实际合约品种不一致")
    # Check the supplied full month; never infer a century from the file code.
    if not result.delivery_month.replace("-", "").endswith(product[3]):
        raise ValueError("文件代码月份与目录的完整交割年月不一致")
