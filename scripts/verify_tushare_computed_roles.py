"""Opt-in current-source role computation acceptance, isolated from the active environment."""

import argparse
import os
import secrets
from datetime import date, timedelta
from pathlib import Path
from typing import Literal

from asterion_bindings import data_sync
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.calendar import TimeVersion
from asterion_bindings.data_sources import SourceCredentials
from asterion_bindings.database import create_engine
from asterion_bindings.roles import RoleQuery
from asterion_bindings.task_repository import Tasks, task_port
from fastapi.testclient import TestClient
from pydantic import AwareDatetime, BaseModel, ConfigDict, model_validator
from verify_tushare import Report, read_connection

from asterion.api.app import create_app
from asterion.contract_roles.candidates import CandidateRequest, CandidateScope, candidate_evidence
from asterion.contract_roles.computed import replay_computed
from asterion.contract_roles.computed_public import ComputedRequest, ComputedVersion, DailyInput
from asterion.contract_roles.plugin import RoleBackup, validate
from asterion.contract_roles.ranking import RankingPolicy
from asterion.data.catalog import snapshots
from asterion.data.providers.public import SyncRequest
from asterion.data.public import snapshot_backup_access
from asterion.data.sync import DataSync
from asterion.distribution_storage import data_storage
from asterion.platform.communication.schema import initialize_core
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.runtime.environment import EnvironmentHost


class ComputationCase(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: Literal[1]
    exchange: str
    product_id: str
    candidate_scope: CandidateScope
    observation_day: date
    policy: RankingPolicy
    initial_main: str | None
    trading_time: TimeVersion
    expected_effective_start: AwareDatetime

    @model_validator(mode="after")
    def scope(self):
        if (
            self.product_id != f"{self.exchange}.{self.trading_time.spec.product}"
            or self.exchange != self.trading_time.spec.exchange
        ):
            raise ValueError("验收品种与时间契约不一致")
        if not any(s.trading_day == self.observation_day for s in self.trading_time.spec.spans()):
            raise ValueError("验收日未覆盖")
        return self


def collect_evidence(sync, credentials, job) -> bytes:
    """Collect a claimed task from Tushare with the entry's Rust collection."""
    return data_sync.collect(
        "sqlite:///" + str(sync.root / "acceptance.db"), credentials, sync.root, job
    )


def suite(report: Report, case: ComputationCase, configuration: dict):
    root = report.root
    key = secrets.token_urlsafe(32)
    with os.fdopen(
        os.open(root / "runtime.key", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w"
    ) as file:
        file.write(key)
    secret = SourceCredentials(key, root)
    engine = create_engine("sqlite:///" + str(root / "acceptance.db"), hide_parameters=True)
    try:
        initialize_core(engine)
        tasks = Tasks(engine, lease_seconds=240)
        storage = data_storage(engine)
        storage.initialize(snapshots)
        sync = DataSync(
            storage, task_port(tasks, frozenset({"data.sync", "data.import_csv"})), root, secret
        )
        sync.sources.apply("tushare", 0, secrets=configuration)
        fixed = {}
        report.value["versions"] = fixed

        def publish(label, dataset, *, symbol="", start=None, end=None):
            request = SyncRequest(
                command_id=label,
                provider="tushare",
                dataset=dataset,
                exchange=case.exchange,
                symbol=symbol,
                start=start,
                end=end,
            )
            accepted = sync.admit(
                request.model_dump()
                | {
                    "contracts_version_id": fixed["contracts"]["id"]
                    if dataset == "daily"
                    else None,
                }
            )
            job = tasks.claim("computed-role-acceptance")
            if job is None or job["id"] != accepted["id"]:
                raise ValueError("验收任务领取失败")
            try:
                content = collect_evidence(sync, secret, job)
                version = sync.publish(job["id"], job["token"], content)
            except Exception:
                tasks.fail(job["id"], job["token"], "验收采集失败；检查分阶段报告")
                raise
            result = {
                "id": version["id"],
                "rows": version["rows"],
                "checksum": version["manifest"]["checksum"],
            }
            fixed[label] = result
            return result

        report.step("contracts", lambda: publish("contracts", "contracts"))
        report.step(
            "calendar",
            lambda: publish(
                "calendar",
                "calendar",
                start=case.trading_time.spec.calendar[0].date,
                end=case.observation_day,
            ),
        )

        def check_calendar():
            value = sync.library.preview(fixed["calendar"]["id"], limit=10000)
            observed = {date.fromisoformat(r["date"]): bool(r["is_open"]) for r in value["rows"]}
            expected = {
                r.date: r.is_open
                for r in case.trading_time.spec.calendar
                if r.date <= case.observation_day
            }
            if len(observed) != value["total"] or observed != expected:
                raise ValueError("来源交易日历与固定验收时间契约不一致")

        report.step("historical_calendar_matches_fixed_time", check_calendar)
        candidates = report.step(
            "candidate_coverage",
            lambda: candidate_evidence(
                sync.library.preview,
                CandidateRequest(
                    contracts_version_id=fixed["contracts"]["id"],
                    product_id=case.product_id,
                    scope=case.candidate_scope,
                    start=case.observation_day,
                    end=case.observation_day,
                ),
            ),
        )
        (root / "candidates.json").write_bytes(canonical(candidates.model_dump(mode="json")))
        symbols = sorted(
            s.symbol for s in candidates.catalog.symbols if s.contract_id in candidates.included
        )
        report.value["candidate_coverage"] = {
            "scope": candidates.coverage,
            "included": len(symbols),
            "excluded": len(candidates.excluded),
            "exchange_completeness_attested": False,
        }
        report.write()
        for symbol in symbols:
            report.step(
                "daily:" + symbol,
                lambda symbol=symbol: publish(
                    symbol,
                    "daily",
                    symbol=symbol,
                    start=case.observation_day,
                    end=case.observation_day,
                ),
            )
        report.write()
        request = ComputedRequest(
            schema_version=1,
            contracts_version_id=fixed["contracts"]["id"],
            candidate_scope=case.candidate_scope,
            product_id=case.product_id,
            trading_time=case.trading_time,
            policy=case.policy,
            initial_main=case.initial_main,
            daily_inputs=tuple(
                DailyInput(trading_day=case.observation_day, version_id=fixed[s]["id"])
                for s in symbols
            ),
            explanation="真实来源固定候选集计算验收；本机可知时间，不认证历史公布时间或全市场主力",
        )
        with TestClient(
            create_app(Settings(token=key, data_root=root, require_account=False), engine)
        ) as client:
            client.headers["Authorization"] = "Bearer " + key
            path = "/api/v1/contract-roles/computed"

            def publish_roles():
                spec = (
                    client.post(path + "/preview", json=request.model_dump(mode="json"))
                    .raise_for_status()
                    .json()
                )
                result = client.post(path, json=spec).raise_for_status().json()
                if client.post(path, json=spec).raise_for_status().json() != result:
                    raise ValueError("重复发布改变计算记录或发布时间")
                return ComputedVersion.model_validate(result)

            version = report.step("verified_computed_publication", publish_roles)
            (root / "computed-roles.json").write_bytes(canonical(version.model_dump(mode="json")))

            def check_time():
                if len(version.spec.result.decisions) != 1:
                    raise ValueError("单交易日验收应产生一个生效决定")
                decision = version.spec.result.decisions[0]
                if (
                    decision.effective_start != case.expected_effective_start
                    or version.published_at > decision.effective_start
                ):
                    raise ValueError("当前采集发布时间不满足固定验收开盘；不得改写采集时间")
                for role in ("main", "secondary"):
                    query = RoleQuery(
                        version_id=version.id,
                        role=role,
                        timestamp=decision.effective_start,
                        information_at=version.published_at,
                        mode="as_known",
                        explanation="未来开盘的固定角色查询，不代表实盘已执行",
                    )
                    resolved = (
                        client.post(path + "/resolve", json=query.model_dump(mode="json"))
                        .raise_for_status()
                        .json()
                    )
                    if (
                        resolved["contract"]["id"] != getattr(decision, role)
                        or resolved["execution_authorized"]
                    ):
                        raise ValueError("计算角色查询结果不一致")
                    too_early = query.model_dump(mode="json") | {
                        "information_at": (
                            version.published_at - timedelta(microseconds=1)
                        ).isoformat()
                    }
                    if client.post(path + "/resolve", json=too_early).status_code != 422:
                        raise ValueError("发布前查询必须失败")
                historical = next(
                    s
                    for s in case.trading_time.spec.spans()
                    if s.trading_day == case.observation_day
                )
                backdated = query.model_dump(mode="json") | {
                    "timestamp": historical.start.isoformat(),
                    "information_at": historical.start.isoformat(),
                }
                if client.post(path + "/resolve", json=backdated).status_code != 422:
                    raise ValueError("新采集信息不能回填观测日")
                return decision.model_dump(mode="json")

            decision = report.step("effective_open_and_no_backdating", check_time)
            replay = (
                client.post(path + "/replay", json=version.spec.model_dump(mode="json"))
                .raise_for_status()
                .json()
            )
            if replay != version.spec.result.model_dump(mode="json"):
                raise ValueError("在线重放不一致")

        def offline():
            frozen = ComputedVersion.model_validate_json(
                (root / "computed-roles.json").read_bytes()
            )
            if replay_computed(frozen.spec) != version.spec.result:
                raise ValueError("离线重放不一致")

        report.step("offline_replay", offline)

        def backup():
            with engine.connect() as conn:
                access = snapshot_backup_access(conn, ArtifactStore(root, read_only=True))
            return validate(RoleBackup((), access, (version.model_dump(mode="json"),), ()))

        report.step("backup_sources", backup)
        report.value.update(
            status="PASSED",
            computed_version=version.id,
            published_at=version.published_at.isoformat(),
            decision=decision,
            historical_knowledge_attested=False,
            exact_reproduction=True,
            live_session_observed=False,
            whole_market_dominance_verified=False,
        )
        report.write()
    finally:
        engine.dispose()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        type=Path,
        default=Path.home() / "Library/Application Support/me.asterion.terminal",
    )
    parser.add_argument("--connection", required=True)
    parser.add_argument("--case", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        case = ComputationCase.model_validate_json(args.case.read_bytes())
        host, output = args.host.resolve(), args.output.resolve()
        with EnvironmentHost(host) as environment:
            state = environment.active()
        if any(output.is_relative_to(p) or p.is_relative_to(output) for p in (host, state)):
            raise ValueError("输出必须与活动环境独立")
        report = Report(output, case)
        configuration, _ = report.step("connection", lambda: read_connection(host, args.connection))
        report.step("acceptance", lambda: suite(report, case, configuration))
        print("计算角色验收通过：" + str(output / "report.json"))
        return 0
    except Exception as exc:  # noqa: BLE001 - never print credential-bearing exceptions
        print("计算角色验收失败（" + type(exc).__name__ + "）；请检查报告中的失败阶段。")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
