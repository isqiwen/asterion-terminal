"""Immutable provider role reports. Roles resolve to contracts; they are not instruments."""

import hashlib
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from typing import Literal

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

from asterion.data.public import Contract, ReferenceCatalog
from asterion.platform.plugins import Capability
from asterion.platform.serialization import canonical
from asterion.trading_time.public import Span, TimeVersion

Role = Literal["main", "secondary"]


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True, str_strip_whitespace=True)


class RoleReport(Strict):
    trading_day: date
    role: Role
    contract_id: str = Field(min_length=1)
    # Missing historical publication time is not filled from the trading-day label.
    available_at: AwareDatetime | None
    evidence: str = Field(min_length=1, max_length=2000)


class RoleSpec(Strict):
    schema_version: Literal[1]
    origin: Literal["provider_report"]
    source: str = Field(min_length=1, max_length=100)
    source_version: str = Field(min_length=1, max_length=100)
    source_checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    observed_at: AwareDatetime
    product_id: str
    catalog: ReferenceCatalog
    trading_time: TimeVersion
    reports: tuple[RoleReport, ...] = Field(min_length=1, max_length=10000)

    @model_validator(mode="after")
    def valid(self):
        timing = self.trading_time.spec
        if self.product_id != f"{timing.exchange}.{timing.product}":
            raise ValueError("角色品种与交易时间不一致")
        contracts = {c.id: c for c in self.catalog.contracts}
        seen = set()
        assignments = set()
        for row in self.reports:
            key = (row.trading_day, row.role)
            if key in seen:
                raise ValueError("同一交易日角色重复或冲突")
            seen.add(key)
            actual = contracts.get(row.contract_id)
            if actual is None or actual.product_id != self.product_id:
                raise ValueError("角色必须指向固定目录中同品种的实际合约")
            if not actual.listed_on <= row.trading_day <= actual.last_trade_on:
                raise ValueError("角色超出实际合约生命周期")
            if not any(s.trading_day == row.trading_day for s in timing.spans()):
                raise ValueError("角色交易日休市或缺少时间覆盖")
            assigned = (row.trading_day, actual.id)
            if assigned in assignments:
                raise ValueError("同一交易日主力与次主力不能指向同一合约")
            assignments.add(assigned)
            if row.available_at is not None and row.available_at > self.observed_at:
                raise ValueError("来源可知时刻不能晚于本次观测时刻")
        if list(self.reports) != sorted(self.reports, key=lambda r: (r.trading_day, r.role)):
            raise ValueError("角色记录必须按交易日和角色排序")
        return self


def role_id(spec: RoleSpec) -> str:
    return hashlib.sha256(canonical(spec.model_dump(mode="json"))).hexdigest()


class RoleVersion(Strict):
    id: str = Field(pattern=r"^[a-f0-9]{64}$")
    spec: RoleSpec

    @model_validator(mode="after")
    def fingerprint(self):
        if self.id != role_id(self.spec):
            raise ValueError("角色版本指纹不一致")
        return self


class RoleQuery(Strict):
    version_id: str = Field(pattern=r"^[a-f0-9]{64}$")
    role: Role
    timestamp: AwareDatetime
    information_at: AwareDatetime
    mode: Literal["as_known", "retrospective"]
    explanation: str = Field(max_length=2000)

    @model_validator(mode="after")
    def policy(self):
        if self.mode == "retrospective" and not self.explanation:
            raise ValueError("回溯查看必须说明历史可知性未经确认")
        if self.mode == "as_known" and self.information_at > self.timestamp:
            raise ValueError("当时可知查询不能使用未来信息截止时间")
        return self


class RoleResolution(Strict):
    version_id: str
    contract: Contract
    report: RoleReport
    session: Span
    mode: Literal["as_known", "retrospective"]
    explanation: str
    # A role resolution never certifies broker execution or provenance authenticity.
    execution_authorized: Literal[False] = False


def resolve(version: RoleVersion, query: RoleQuery) -> RoleResolution:
    if query.version_id != version.id:
        raise ValueError("角色查询版本不一致")
    spec = version.spec
    spans = [s for s in spec.trading_time.spec.spans() if s.start <= query.timestamp < s.end]
    if len(spans) != 1:
        raise ValueError("查询时刻不在固定交易时段中")
    session = spans[0]
    rows = [
        r for r in spec.reports if r.trading_day == session.trading_day and r.role == query.role
    ]
    if len(rows) != 1:
        raise ValueError("角色映射缺口；不能沿用上一交易日")
    row = rows[0]
    actual = next(c for c in spec.catalog.contracts if c.id == row.contract_id)
    if query.mode == "as_known":
        if row.available_at is None or row.available_at > query.information_at:
            raise ValueError("角色历史可知时间未知或晚于信息截止时间")
    elif spec.observed_at > query.information_at:
        raise ValueError("映射尚未在信息截止时间前观测到")
    product = next(p for p in spec.catalog.products if p.id == actual.product_id)
    if max(actual.provenance.available_at, product.provenance.available_at) > query.information_at:
        raise ValueError("合约身份资料晚于信息截止时间")
    return RoleResolution(
        version_id=version.id,
        contract=actual,
        report=row,
        session=session,
        mode=query.mode,
        explanation=query.explanation,
    )


class NextOpening(Strict):
    product_id: str
    trading_time: TimeVersion
    observation_end: AwareDatetime
    available_at: AwareDatetime


def next_opening(body: NextOpening) -> Span:
    """First trading-day opening after the observation, using explicit night/calendar rules."""
    timing = body.trading_time.spec
    if body.product_id != f"{timing.exchange}.{timing.product}":
        raise ValueError("观测品种与交易时间不一致")
    if body.available_at < body.observation_end:
        raise ValueError("完整观测窗口结束前不能宣称结果可知")
    spans = timing.spans()
    if not spans or not spans[0].start <= body.observation_end <= spans[-1].end:
        raise ValueError("固定时段未覆盖观测窗口结束时刻")
    starts = {}
    for span in spans:
        if span.phase == "continuous":
            starts.setdefault(span.trading_day, span)
    for span in sorted(starts.values(), key=lambda s: s.start):
        if span.start > body.observation_end and span.start >= body.available_at:
            return span
    raise ValueError("固定日历和时段未覆盖下一生效开盘；不能推算或顺延")


@dataclass(frozen=True)
class RoleAccess:
    read: Callable[[str], RoleVersion]


ROLE_ACCESS = Capability("contract_roles.read", "asterion.contract_roles", RoleAccess)
