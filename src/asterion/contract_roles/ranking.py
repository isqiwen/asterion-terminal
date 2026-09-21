"""Explicit daily role ranking diagnostics; supplied evidence is not source-certified."""

import hashlib
from datetime import date
from decimal import Decimal
from fractions import Fraction
from typing import Literal

from pydantic import AwareDatetime, Field

from asterion.contract_roles.public import NextOpening, Strict, next_opening
from asterion.data.public import ReferenceCatalog
from asterion.platform.serialization import canonical
from asterion.trading_time.public import TimeVersion


class RankingPolicy(Strict):
    metric: Literal["volume", "open_interest"]
    unit: Literal["contracts"]
    tie_break: Literal["earlier_delivery", "later_delivery"]
    missing: Literal["reject"]
    no_trade: Literal["reject", "exclude"]
    switch_margin: Decimal = Field(ge=0, max_digits=24, decimal_places=12, allow_inf_nan=False)
    confirmations: int = Field(ge=1, le=100)
    allow_backward: bool
    secondary: Literal["best_remaining"]


class RankingValue(Strict):
    contract_id: str = Field(min_length=1)
    volume: Decimal = Field(ge=0, max_digits=24, decimal_places=12, allow_inf_nan=False)
    open_interest: Decimal = Field(ge=0, max_digits=24, decimal_places=12, allow_inf_nan=False)
    source_version: str = Field(min_length=1)
    source_checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    available_at: AwareDatetime


class RankingDay(Strict):
    trading_day: date
    values: tuple[RankingValue, ...] = Field(min_length=1, max_length=1000)


class RankingRequest(Strict):
    schema_version: Literal[1]
    product_id: str
    catalog: ReferenceCatalog
    trading_time: TimeVersion
    candidates: tuple[str, ...] = Field(min_length=2, max_length=1000)
    policy: RankingPolicy
    initial_main: str | None
    observations: tuple[RankingDay, ...] = Field(min_length=1, max_length=1000)
    evidence_scope: Literal["unverified_diagnostic"]
    explanation: str = Field(min_length=1, max_length=2000)


class RankingExclusion(Strict):
    contract_id: str
    reason: Literal["not_listed", "expired", "expires_before_effective", "no_trade"]


class RankingDecision(Strict):
    observation_day: date
    observation_start: AwareDatetime
    observation_end: AwareDatetime
    available_at: AwareDatetime
    effective_day: date
    effective_start: AwareDatetime
    effective_end: AwareDatetime
    ranking: tuple[str, ...]
    excluded: tuple[RankingExclusion, ...]
    main: str
    secondary: str
    challenger: str | None
    confirmation_count: int
    reason: Literal["initial", "retained", "confirming", "switched"]


class RankingResult(Strict):
    input_digest: str
    algorithm: Literal["daily-role-ranking.v1"]
    decisions: tuple[RankingDecision, ...]
    evidence_scope: Literal["unverified_diagnostic"] = "unverified_diagnostic"
    execution_authorized: Literal[False] = False
    publishable: Literal[False] = False


def rank_roles(body: RankingRequest) -> RankingResult:
    """Replay a complete consecutive observation window; never carry missing daily inputs."""
    policy = body.policy
    catalog = {c.id: c for c in body.catalog.contracts}
    candidates = set(body.candidates)
    if len(candidates) != len(body.candidates) or not candidates <= catalog.keys():
        raise ValueError("候选集合重复或缺少规范合约身份")
    if any(catalog[c].product_id != body.product_id for c in candidates):
        raise ValueError("候选合约必须属于同一品种")
    if body.initial_main is not None and body.initial_main not in candidates:
        raise ValueError("初始主力不在候选集合")
    product = next(p for p in body.catalog.products if p.id == body.product_id)
    spans = body.trading_time.spec.spans()
    days = sorted({s.trading_day for s in spans})
    observed = [o.trading_day for o in body.observations]
    if observed != sorted(set(observed)) or any(d not in days for d in observed):
        raise ValueError("观测交易日重复、无序或未覆盖")
    if observed != days[days.index(observed[0]) : days.index(observed[-1]) + 1]:
        raise ValueError("观测交易日存在缺口；不能跨缺口累计确认")
    main = body.initial_main
    challenger = None
    count = 0
    decisions = []
    for observation in body.observations:
        day = observation.trading_day
        sessions = [s for s in spans if s.trading_day == day]
        start, end = min(s.start for s in sessions), max(s.end for s in sessions)
        active = {c for c in candidates if catalog[c].listed_on <= day <= catalog[c].last_trade_on}
        values = {v.contract_id: v for v in observation.values}
        if len(values) != len(observation.values) or values.keys() != active:
            raise ValueError("观测必须精确覆盖候选集合中当日全部存续合约，不能缺失或重复")
        if any(v.available_at < end for v in values.values()):
            raise ValueError("完整日线指标不能在观测收盘前可知")
        available = max(
            [v.available_at for v in values.values()]
            + [catalog[c].provenance.available_at for c in candidates]
            + [product.provenance.available_at]
        )
        opening = next_opening(
            NextOpening(
                product_id=body.product_id,
                trading_time=body.trading_time,
                observation_end=end,
                available_at=available,
            )
        )
        if decisions and opening.trading_day <= decisions[-1].effective_day:
            raise ValueError("延迟观测产生重叠或倒序生效日；不能覆盖先前决定")
        eligible = {c for c in active if catalog[c].last_trade_on >= opening.trading_day}
        idle = {c for c in eligible if values[c].volume == 0}
        if idle and policy.no_trade == "reject":
            raise ValueError("候选合约无成交；规则要求拒绝")
        eligible -= idle
        exclusions = []
        for identifier in sorted(candidates - eligible):
            actual = catalog[identifier]
            reason = (
                "not_listed"
                if actual.listed_on > day
                else "expired"
                if actual.last_trade_on < day
                else "expires_before_effective"
                if actual.last_trade_on < opening.trading_day
                else "no_trade"
            )
            exclusions.append(RankingExclusion(contract_id=identifier, reason=reason))
        if len(eligible) < 2:
            raise ValueError("生效日缺少两个有效且有成交的候选合约")
        if main is not None and main not in eligible:
            raise ValueError("原主力失效或无成交；必须明确处理，不能自动强制换月")
        # Stable canonical ID resolves equal delivery months; input order has no effect.
        ranked = sorted(eligible)
        ranked.sort(
            key=lambda c: catalog[c].delivery_month, reverse=policy.tie_break == "later_delivery"
        )
        ranked.sort(key=lambda c: getattr(values[c], policy.metric), reverse=True)
        reason = "retained"
        if main is None:
            main, reason = ranked[0], "initial"
        else:
            allowed = [
                c
                for c in ranked
                if c != main
                and (
                    policy.allow_backward
                    or catalog[c].delivery_month >= catalog[main].delivery_month
                )
            ]
            best = allowed[0] if allowed else None
            current_score = Fraction(getattr(values[main], policy.metric))
            qualifies = best is not None and Fraction(getattr(values[best], policy.metric)) > (
                current_score * (1 + Fraction(policy.switch_margin))
            )
            if qualifies:
                count = count + 1 if challenger == best else 1
                challenger = best
                reason = "confirming"
                if count >= policy.confirmations:
                    main, reason = best, "switched"
            else:
                challenger, count = None, 0
        assert main is not None
        decisions.append(
            RankingDecision(
                observation_day=day,
                observation_start=start,
                observation_end=end,
                available_at=available,
                effective_day=opening.trading_day,
                effective_start=opening.start,
                effective_end=max(s.end for s in spans if s.trading_day == opening.trading_day),
                ranking=tuple(ranked),
                excluded=tuple(exclusions),
                main=main,
                secondary=next(c for c in ranked if c != main),
                challenger=challenger,
                confirmation_count=count,
                reason=reason,
            )
        )
        if reason == "switched":
            challenger, count = None, 0
    return RankingResult(
        input_digest=hashlib.sha256(canonical(body.model_dump(mode="json"))).hexdigest(),
        algorithm="daily-role-ranking.v1",
        decisions=tuple(decisions),
    )
