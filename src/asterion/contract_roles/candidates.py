"""Explicit candidate scope and reproducible lifecycle exclusions, owned by the role plugin."""

from datetime import date
from typing import Literal

from pydantic import Field, model_validator

from asterion.contract_roles.public import Strict
from asterion.data.public import ReferenceCatalog, source_contract_catalog, source_product_catalog


class CandidateScope(Strict):
    mode: Literal["explicit", "product_catalog"]
    symbols: tuple[str, ...] = Field(max_length=1000)

    @model_validator(mode="after")
    def selection(self):
        if self.mode == "explicit" and (
            len(self.symbols) < 2 or len(set(self.symbols)) != len(self.symbols)
        ):
            raise ValueError("显式候选至少两个，且不能重复")
        if self.mode == "product_catalog" and self.symbols:
            raise ValueError("品种目录范围由固定资料决定，不能另填候选子集")
        return self


class CandidateRequest(Strict):
    contracts_version_id: str = Field(min_length=1, max_length=100)
    product_id: str
    scope: CandidateScope
    start: date
    end: date

    @model_validator(mode="after")
    def window(self):
        if self.end < self.start:
            raise ValueError("候选观测窗口倒序")
        return self


class CandidateExclusion(Strict):
    contract_id: str
    symbol: str
    reason: Literal["expired_before_window", "listed_after_window"]


class CandidateEvidence(Strict):
    request: CandidateRequest
    catalog: ReferenceCatalog
    included: tuple[str, ...]
    excluded: tuple[CandidateExclusion, ...]
    coverage: Literal["explicit_subset", "fixed_source_product"]
    exchange_completeness_attested: Literal[False] = False


def candidate_evidence(reader, body: CandidateRequest) -> CandidateEvidence:
    catalog = (
        source_product_catalog(reader, body.contracts_version_id, body.product_id)
        if body.scope.mode == "product_catalog"
        else source_contract_catalog(reader, body.contracts_version_id, sorted(body.scope.symbols))
    )
    if any(c.product_id != body.product_id for c in catalog.contracts):
        raise ValueError("候选目录包含其他品种")
    included = []
    excluded = []
    for actual in catalog.contracts:
        mappings = [s for s in catalog.symbols if s.contract_id == actual.id]
        if len(mappings) != 1:
            raise ValueError("候选实际合约须有唯一来源代码")
        if actual.last_trade_on < body.start:
            reason = "expired_before_window"
        elif actual.listed_on > body.end:
            reason = "listed_after_window"
        else:
            included.append(actual.id)
            continue
        excluded.append(
            CandidateExclusion(contract_id=actual.id, symbol=mappings[0].symbol, reason=reason)
        )
    if len(included) < 2:
        raise ValueError("观测窗口缺少两个存续候选合约")
    return CandidateEvidence(
        request=body,
        catalog=catalog,
        included=tuple(sorted(included)),
        excluded=tuple(sorted(excluded, key=lambda item: item.contract_id)),
        coverage="fixed_source_product"
        if body.scope.mode == "product_catalog"
        else "explicit_subset",
    )
