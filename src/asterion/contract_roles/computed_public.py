"""Fixed computed role records, distinct from provider declarations."""

import hashlib
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from typing import Literal

from pydantic import AwareDatetime, ConfigDict, Field, model_validator

from asterion.contract_roles.candidates import CandidateEvidence, CandidateScope
from asterion.contract_roles.public import RoleQuery, Strict
from asterion.contract_roles.ranking import (
    RankingDecision,
    RankingPolicy,
    RankingRequest,
    RankingResult,
)
from asterion.data.public import Contract
from asterion.platform.plugins import Capability
from asterion.platform.serialization import canonical
from asterion.trading_time.public import Span, TimeVersion


def digest(value) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


class DailyInput(Strict):
    trading_day: date
    version_id: str = Field(min_length=1, max_length=100)


class ComputedRequest(Strict):
    schema_version: Literal[1]
    contracts_version_id: str = Field(min_length=1, max_length=100)
    candidate_scope: CandidateScope
    product_id: str
    trading_time: TimeVersion
    policy: RankingPolicy
    initial_main: str | None
    daily_inputs: tuple[DailyInput, ...] = Field(min_length=2, max_length=10000)
    explanation: str = Field(min_length=1, max_length=2000)


class AlgorithmArtifact(Strict):
    model_config = ConfigDict(extra="forbid", frozen=True, str_strip_whitespace=False)
    algorithm: Literal["daily-role-ranking.v1"]
    sources: dict[str, str]
    runtime: dict[str, str]
    checksum: str = Field(pattern=r"^[a-f0-9]{64}$")

    @model_validator(mode="after")
    def fingerprint(self):
        if self.checksum != digest(self.model_dump(mode="json", exclude={"checksum"})):
            raise ValueError("排名算法制品指纹不一致")
        return self


class ComputedSpec(Strict):
    schema_version: Literal[1]
    origin: Literal["computed"]
    request: ComputedRequest
    candidates: CandidateEvidence
    input: RankingRequest
    artifact: AlgorithmArtifact
    result: RankingResult
    availability_basis: Literal["local_observation"]
    historical_publication_attested: Literal[False]
    execution_authorized: Literal[False]

    @model_validator(mode="after")
    def fingerprints(self):
        if self.result.input_digest != digest(self.input.model_dump(mode="json")):
            raise ValueError("计算输入与结果指纹不一致")
        if self.result.algorithm != self.artifact.algorithm:
            raise ValueError("计算结果与算法制品不一致")
        return self


class ComputedVersion(Strict):
    id: str = Field(pattern=r"^[a-f0-9]{64}$")
    spec: ComputedSpec
    published_at: AwareDatetime

    @model_validator(mode="after")
    def fingerprint(self):
        if self.id != digest(self.spec.model_dump(mode="json")):
            raise ValueError("计算角色版本指纹不一致")
        if any(d.available_at > self.published_at for d in self.spec.result.decisions):
            raise ValueError("不能在计算依据可知之前发布角色")
        return self


class ComputedResolution(Strict):
    version_id: str
    contract: Contract
    decision: RankingDecision
    session: Span
    available_at: AwareDatetime
    availability_basis: Literal["local_observation"] = "local_observation"
    historical_publication_attested: Literal[False] = False
    execution_authorized: Literal[False] = False


def resolve_computed(version: ComputedVersion, query: RoleQuery) -> ComputedResolution:
    if query.version_id != version.id or query.mode != "as_known":
        raise ValueError("计算角色要求精确版本与本机当时可知查询，不提供假定历史回填")
    spec = version.spec
    sessions = [
        s for s in spec.input.trading_time.spec.spans() if s.start <= query.timestamp < s.end
    ]
    if len(sessions) != 1:
        raise ValueError("查询时刻不在固定交易时段中")
    session = sessions[0]
    decisions = [d for d in spec.result.decisions if d.effective_day == session.trading_day]
    if len(decisions) != 1 or query.timestamp < decisions[0].effective_start:
        raise ValueError("计算角色尚未生效或存在缺口，不能沿用前值")
    decision = decisions[0]
    if version.published_at > decision.effective_start:
        raise ValueError("角色发布晚于计划生效开盘，不能回填或盘中启用该日角色")
    if max(decision.available_at, version.published_at) > query.information_at:
        raise ValueError("计算依据晚于信息截止时间")
    identifier = decision.main if query.role == "main" else decision.secondary
    actual = next(c for c in spec.input.catalog.contracts if c.id == identifier)
    return ComputedResolution(
        version_id=version.id,
        contract=actual,
        decision=decision,
        session=session,
        available_at=max(decision.available_at, version.published_at),
    )


@dataclass(frozen=True)
class ComputedRoleAccess:
    read: Callable[[str], ComputedVersion]


COMPUTED_ROLE_ACCESS = Capability(
    "contract_roles.computed.read", "asterion.contract_roles", ComputedRoleAccess
)
