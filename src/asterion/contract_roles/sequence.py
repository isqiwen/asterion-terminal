"""Consecutive publication evidence within one fixed role-computation campaign."""

from typing import Literal

from pydantic import Field, model_validator

from asterion.contract_roles.computed import ComputedSources
from asterion.contract_roles.computed_public import (
    ComputedRequest,
    ComputedSpec,
    ComputedVersion,
    DailyInput,
    resolve_computed,
)
from asterion.contract_roles.public import RoleQuery, Strict


class ContinuationRequest(Strict):
    previous_version_id: str = Field(pattern=r"^[a-f0-9]{64}$")
    daily_inputs: tuple[DailyInput, ...] = Field(min_length=1, max_length=1000)
    explanation: str = Field(min_length=1, max_length=2000)


class SequenceRequest(Strict):
    version_ids: tuple[str, ...] = Field(min_length=2, max_length=50)
    minimum_switches: int = Field(ge=0, le=49)

    @model_validator(mode="after")
    def unique(self):
        if len(set(self.version_ids)) != len(self.version_ids):
            raise ValueError("连续发布验收版本不能重复")
        return self


class SequenceResult(Strict):
    version_ids: tuple[str, ...]
    observation_days: tuple[str, ...]
    switches: int
    source_verified: Literal[True] = True
    execution_authorized: Literal[False] = False
    historical_publication_attested: Literal[False] = False


def check_extension(previous: ComputedSpec, current: ComputedSpec):
    # A campaign freezes identity, calendar, policy and initial state. Never reset confirmation.
    omit = {"daily_inputs", "explanation"}
    if (
        previous.request.model_dump(exclude=omit) != current.request.model_dump(exclude=omit)
        or previous.artifact != current.artifact
        or previous.input.catalog != current.input.catalog
        or previous.input.candidates != current.input.candidates
    ):
        raise ValueError("连续发布必须保持固定资料、日历、候选、算法、规则和初始状态")
    old = previous.input.observations
    new = current.input.observations
    refs = {(r.trading_day, r.version_id) for r in previous.request.daily_inputs}
    retained = {
        (r.trading_day, r.version_id)
        for r in current.request.daily_inputs
        if r.trading_day <= old[-1].trading_day
    }
    if len(new) != len(old) + 1 or new[:-1] != old or retained != refs:
        raise ValueError("连续发布只能追加一个交易日，不能替换固定历史输入或跳日")
    if current.result.decisions[:-1] != previous.result.decisions:
        raise ValueError("连续发布改变了先前排名或确认计数")


def check_published(version: ComputedVersion):
    decision = version.spec.result.decisions[-1]
    for role in ("main", "secondary"):
        resolve_computed(
            version,
            RoleQuery(
                version_id=version.id,
                role=role,
                timestamp=decision.effective_start,
                information_at=version.published_at,
                mode="as_known",
                explanation="连续发布时点核验，不代表已执行交易",
            ),
        )


def continuation(sources: ComputedSources, previous: ComputedVersion, body: ContinuationRequest):
    if previous.id != body.previous_version_id:
        raise ValueError("续算版本与请求不一致")
    sources.verify(previous.spec)
    check_published(previous)
    last = previous.spec.input.observations[-1].trading_day
    if len({r.trading_day for r in body.daily_inputs}) != 1 or any(
        r.trading_day <= last for r in body.daily_inputs
    ):
        raise ValueError("续算只接受一个新的观测交易日")
    request = ComputedRequest.model_validate(
        previous.spec.request.model_dump()
        | {
            "daily_inputs": (*previous.spec.request.daily_inputs, *body.daily_inputs),
            "explanation": body.explanation,
        }
    )
    result = sources.build(request)
    check_extension(previous.spec, result)
    return result


def verify_sequence(sources: ComputedSources, read, body: SequenceRequest) -> SequenceResult:
    records = []
    switches = 0
    for identifier in body.version_ids:
        record = read(identifier)
        if record.id != identifier:
            raise ValueError("连续验收读取版本不一致")
        sources.verify(record.spec)
        check_published(record)
        if records:
            previous = records[-1]
            if record.published_at <= previous.published_at:
                raise ValueError("连续发布时刻必须递增")
            check_extension(previous.spec, record.spec)
            switches += (
                previous.spec.result.decisions[-1].main != record.spec.result.decisions[-1].main
            )
        elif len(record.spec.input.observations) != 1:
            raise ValueError("连续验收必须从首个单日发布开始")
        records.append(record)
    if switches < body.minimum_switches:
        raise ValueError("实际角色切换次数不足，不能将无换月序列标记为换月验收通过")
    return SequenceResult(
        version_ids=body.version_ids,
        observation_days=tuple(str(r.spec.input.observations[-1].trading_day) for r in records),
        switches=switches,
    )
