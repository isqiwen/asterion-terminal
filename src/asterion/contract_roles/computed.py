"""Server reconstruction and replay of source-verified, locally observable role calculations."""

import hashlib
import platform
from importlib.metadata import version
from importlib.resources import files

from asterion.contract_roles.candidates import CandidateRequest, candidate_evidence
from asterion.contract_roles.computed_public import (
    AlgorithmArtifact,
    ComputedRequest,
    ComputedSpec,
    digest,
)
from asterion.contract_roles.ranking import RankingDay, RankingRequest, RankingValue, rank_roles
from asterion.data.public import (
    ResolutionRequest,
    VersionAccess,
    fixed_daily_evidence,
)


def algorithm_artifact() -> AlgorithmArtifact:
    paths = (
        "contract_roles/ranking.py",
        "contract_roles/models.py",
        "platform/serialization.py",
    )
    payload = {
        "algorithm": "daily-role-ranking.v1",
        "sources": {
            path: files("asterion").joinpath(path).read_text(encoding="utf-8") for path in paths
        },
        "runtime": {"python": platform.python_version(), "pydantic": version("pydantic")},
    }
    # Pin the implementation actually executing the domain rules, including Rust.
    from pathlib import Path

    from asterion_bindings import _native

    native = Path(_native.__file__)
    for name in ("roles.py", "_roles_binding.py"):
        payload["sources"][f"asterion_bindings/{name}"] = (
            files("asterion_bindings").joinpath(name).read_text(encoding="utf-8")
        )
    payload["sources"]["asterion_bindings/_native"] = hashlib.sha256(
        native.read_bytes()
    ).hexdigest()
    return AlgorithmArtifact.model_validate(payload | {"checksum": digest(payload)})


def replay_computed(spec: ComputedSpec):
    if spec.artifact != algorithm_artifact():
        raise ValueError("排名算法制品与当前执行器不一致；不执行历史或外来代码")
    if rank_roles(spec.input) != spec.result:
        raise ValueError("计算角色结果无法精确重放")
    return spec.result


class ComputedSources:
    def __init__(self, versions: VersionAccess):
        self.versions = versions

    def build(self, request: ComputedRequest) -> ComputedSpec:
        cache = {}

        def reader(identifier, *, limit):
            if (identifier, limit) not in cache:
                cache[identifier, limit] = self.versions.read(identifier, limit=limit)
            return cache[identifier, limit]

        candidates = candidate_evidence(
            reader,
            CandidateRequest(
                contracts_version_id=request.contracts_version_id,
                product_id=request.product_id,
                scope=request.candidate_scope,
                start=min(r.trading_day for r in request.daily_inputs),
                end=max(r.trading_day for r in request.daily_inputs),
            ),
        )
        catalog = candidates.catalog
        symbols = {s.symbol for s in catalog.symbols if s.contract_id in candidates.included}
        seen = set()
        days = {}
        evidence = {}
        for ref in sorted(request.daily_inputs, key=lambda r: (r.trading_day, r.version_id)):
            if ref.version_id not in evidence:
                evidence[ref.version_id] = fixed_daily_evidence(reader, ref.version_id)
            data = evidence[ref.version_id]
            if (
                data.source != catalog.inputs[0].source
                or data.identity.catalog.inputs[0].version_id != request.contracts_version_id
                or data.identity.symbol not in symbols
            ):
                raise ValueError("计算日线与指定候选资料来源或版本不一致")
            observations = [r for r in data.observations if r.bar.trading_day == ref.trading_day]
            if len(observations) != 1:
                raise ValueError("固定日线缺少唯一观测交易日")
            row = observations[0]
            if row.bar.oi is None:
                raise ValueError("排名日线缺少持仓量；不能填零")
            contract = catalog.resolve(
                ResolutionRequest(
                    source=data.source,
                    symbol=data.identity.symbol,
                    trading_day=ref.trading_day,
                    information_at=max(c.provenance.available_at for c in catalog.contracts),
                )
            ).contract
            if data.identity.resolve(ref.trading_day) != contract:
                raise ValueError("日线实际合约与计算候选身份不一致")
            key = (ref.trading_day, contract.id)
            if key in seen:
                raise ValueError("同一候选交易日不能选择多个输入版本")
            seen.add(key)
            days.setdefault(ref.trading_day, []).append(
                RankingValue(
                    contract_id=contract.id,
                    volume=row.bar.vol,
                    open_interest=row.bar.oi,
                    source_version=data.version_id,
                    source_checksum=data.checksum,
                    available_at=row.available_at,
                )
            )
        body = RankingRequest(
            schema_version=1,
            product_id=request.product_id,
            catalog=catalog,
            trading_time=request.trading_time,
            # Freeze the selected catalog, not the first observation's active subset.
            # Ranking filters each day by lifecycle, so later listings cannot change
            # earlier exclusions or reset the campaign's confirmation history.
            candidates=tuple(sorted(c.id for c in catalog.contracts)),
            policy=request.policy,
            initial_main=request.initial_main,
            observations=tuple(
                RankingDay(trading_day=day, values=tuple(sorted(rows, key=lambda r: r.contract_id)))
                for day, rows in sorted(days.items())
            ),
            evidence_scope="unverified_diagnostic",
            explanation=request.explanation,
        )
        return ComputedSpec(
            schema_version=1,
            origin="computed",
            request=request,
            candidates=candidates,
            input=body,
            artifact=algorithm_artifact(),
            result=rank_roles(body),
            availability_basis="local_observation",
            historical_publication_attested=False,
            execution_authorized=False,
        )

    def verify(self, spec: ComputedSpec):
        replay_computed(spec)
        if self.build(spec.request) != spec:
            raise ValueError("计算角色与固定来源证据不一致")
