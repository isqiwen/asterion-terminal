"""Fixed computed role records, distinct from provider declarations."""

import hashlib
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from datetime import date
from functools import cached_property
from typing import Any, Literal, Self

from asterion_bindings.calendar import TimeVersion
from asterion_bindings.plugin_host import Capability
from asterion_bindings.roles import (
    ComputedAssignment,
    ComputedRoleIndex,
    ComputedRoleResolution,
    RoleQuery,
)
from pydantic import AwareDatetime, ConfigDict, Field, model_validator

from asterion.contract_roles.candidates import CandidateEvidence, CandidateScope
from asterion.contract_roles.models import Strict
from asterion.contract_roles.ranking import (
    RankingDecision,
    RankingPolicy,
    RankingRequest,
    RankingResult,
)
from asterion.platform.serialization import canonical


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
        _ = self._role_index
        return self

    @cached_property
    def _role_index(self) -> ComputedRoleIndex:
        return ComputedRoleIndex(
            version_id=self.id,
            product_id=self.spec.input.product_id,
            catalog=self.spec.input.catalog,
            trading_time=self.spec.input.trading_time,
            published_at=self.published_at,
            records=tuple(
                ComputedAssignment.model_validate(
                    decision.model_dump(include=set(ComputedAssignment.model_fields))
                )
                for decision in self.spec.result.decisions
            ),
        )

    def model_copy(self, *, update: Mapping[str, Any] | None = None, deep: bool = False) -> Self:
        # Never reuse an index compiled for a different publication timestamp.
        return type(self).model_validate(self.model_dump() | dict(update or {}))


class ComputedResolution(ComputedRoleResolution):
    decision: RankingDecision


def resolve_computed(version: ComputedVersion, query: RoleQuery) -> ComputedResolution:
    resolved = version._role_index.resolve(query)
    return ComputedResolution(
        **resolved.model_dump(),
        decision=version.spec.result.decisions[resolved.record_index],
    )


@dataclass(frozen=True)
class ComputedRoleAccess:
    read: Callable[[str], ComputedVersion]


COMPUTED_ROLE_ACCESS = Capability(
    "contract_roles.computed.read", "asterion.contract_roles", ComputedRoleAccess
)
