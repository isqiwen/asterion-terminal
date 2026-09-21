"""Opt-in real role-source acceptance in a fresh directory; never prints credentials."""

import argparse
import os
import secrets
from datetime import UTC, date, datetime
from itertools import pairwise
from pathlib import Path
from typing import Literal

from fastapi.testclient import TestClient
from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import create_engine
from verify_tushare import Report, read_connection

from asterion.api.app import create_app
from asterion.contract_roles.plugin import RoleBackup
from asterion.contract_roles.plugin import validate as validate_roles_backup
from asterion.contract_roles.public import RoleQuery, RoleVersion, resolve
from asterion.contract_roles.sources import RoleSourceRequest
from asterion.data.catalog import snapshots
from asterion.data.configuration import ConfigurationUpdate
from asterion.data.providers.public import SyncRequest
from asterion.data.public import CREDENTIAL_SCOPE, snapshot_backup_access
from asterion.data.sync import DataSync, collect
from asterion.distribution_storage import data_storage
from asterion.platform.config import Settings
from asterion.platform.files import read_files
from asterion.platform.secrets import secret_port
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs
from asterion.platform.task_port import task_port
from asterion.platform.tasks.service import Tasks
from asterion.runtime.environments import active
from asterion.trading_time.public import TimeVersion


class MappingCase(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: Literal[1]
    exchange: str
    symbol: str
    start: date
    end: date
    trading_time: TimeVersion
    minimum_transitions: int = Field(ge=0)


def verify_transitions(version: RoleVersion, case: MappingCase):
    rows = [r for r in version.spec.reports if r.role == "main"]
    expected = {
        s.trading_day
        for s in case.trading_time.spec.spans()
        if case.start <= s.trading_day <= case.end
    }
    if {r.trading_day for r in rows} != expected:
        raise ValueError("验收窗口主力映射未完整覆盖固定交易日历")
    changes = []
    for previous, current in pairwise(rows):
        if previous.contract_id != current.contract_id:
            opening = next(
                s
                for s in case.trading_time.spec.spans()
                if s.trading_day == current.trading_day and s.phase == "continuous"
            )
            changes.append(
                {
                    "trading_day": current.trading_day.isoformat(),
                    "from": previous.contract_id,
                    "to": current.contract_id,
                    "session_start": opening.start.isoformat(),
                }
            )
    if len(changes) < case.minimum_transitions:
        raise ValueError("实际映射变化次数不足，不能将无换月样本标记为换月验收通过")
    return changes


def suite(report, case, configuration):
    root = report.root
    key = secrets.token_urlsafe(32)
    with os.fdopen(
        os.open(root / "runtime.key", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w"
    ) as file:
        file.write(key)
    port = secret_port(key, CREDENTIAL_SCOPE)
    engine = create_engine("sqlite:///" + str(root / "acceptance.db"), hide_parameters=True)
    try:
        jobs.create(engine)
        tasks = Tasks(engine, lease_seconds=240)
        store = data_storage(engine)
        store.initialize(snapshots)
        sync = DataSync(
            store, task_port(tasks, frozenset({"data.sync", "data.import_csv"})), root, port
        )
        sync.configuration.apply(
            "tushare", ConfigurationUpdate(expected_revision=0, values={}, secrets=configuration)
        )
        fixed = {}
        for dataset in ("contracts", "mapping"):

            def publish(dataset=dataset):
                request = SyncRequest(
                    command_id="roles-" + dataset,
                    provider="tushare",
                    dataset=dataset,
                    exchange=case.exchange,
                    symbol=case.symbol if dataset == "mapping" else "",
                    start=case.start if dataset == "mapping" else None,
                    end=case.end if dataset == "mapping" else None,
                )
                accepted = sync.submit(request)
                job = tasks.claim("roles-acceptance")
                if job is None or job["id"] != accepted["id"]:
                    raise ValueError("验收任务领取失败")
                content = collect(job["payload"], root, port)
                version = sync.publish(job["id"], job["token"], content)
                return {
                    "id": version["id"],
                    "rows": version["rows"],
                    "checksum": version["manifest"]["checksum"],
                }

            fixed[dataset] = report.step(dataset, publish)
        report.value["versions"] = fixed
        with TestClient(
            create_app(Settings(token=key, data_root=root, require_account=False), engine)
        ) as client:
            client.headers["Authorization"] = "Bearer " + key

            def publication():
                body = RoleSourceRequest(
                    mapping_version_id=fixed["mapping"]["id"],
                    contracts_version_id=fixed["contracts"]["id"],
                    trading_time=case.trading_time,
                )
                spec = (
                    client.post(
                        "/api/v1/contract-roles/source/preview", json=body.model_dump(mode="json")
                    )
                    .raise_for_status()
                    .json()
                )
                if any(row["available_at"] is not None for row in spec["reports"]):
                    raise ValueError("Tushare 未提供的历史公布时刻不能补造")
                saved = client.post("/api/v1/contract-roles", json=spec).raise_for_status().json()
                return RoleVersion.model_validate(saved)

            version = report.step("verified_publication", publication)
            transitions = report.step(
                "mapping_transitions", lambda: verify_transitions(version, case)
            )

            def queries():
                results = []
                for row in version.spec.reports:
                    span = next(
                        s
                        for s in case.trading_time.spec.spans()
                        if s.trading_day == row.trading_day and s.phase == "continuous"
                    )
                    query = RoleQuery(
                        version_id=version.id,
                        role=row.role,
                        timestamp=span.start,
                        information_at=datetime.now(UTC),
                        mode="retrospective",
                        explanation="供应商历史映射回溯；历史公布时间未知",
                    )
                    response = (
                        client.post(
                            "/api/v1/contract-roles/resolve", json=query.model_dump(mode="json")
                        )
                        .raise_for_status()
                        .json()
                    )
                    strict = query.model_dump(mode="json") | {
                        "mode": "as_known",
                        "information_at": span.start.isoformat(),
                        "explanation": "",
                    }
                    if (
                        client.post("/api/v1/contract-roles/resolve", json=strict).status_code
                        != 422
                    ):
                        raise ValueError("未知历史公布时间必须拒绝当时可知查询")
                    results.append((query, response))
                return results

            expected = report.step("queries_and_unknown_time_rejection", queries)
            (root / "roles.json").write_bytes(canonical(version.model_dump(mode="json")))

        def replay():
            frozen = RoleVersion.model_validate_json((root / "roles.json").read_bytes())
            if any(
                resolve(frozen, query).model_dump(mode="json") != result
                for query, result in expected
            ):
                raise ValueError("离线角色解析不一致")

        report.step("offline_replay", replay)

        def verify_backup():
            with engine.connect() as conn:
                access = snapshot_backup_access(conn, read_files(root))
            return validate_roles_backup(
                RoleBackup((version.model_dump(mode="json"),), access, (), ())
            )

        report.step("backup_sources", verify_backup)
        report.value.update(
            status="PASSED",
            role_version=version.id,
            reports=len(version.spec.reports),
            actual_contracts=sorted({r.contract_id for r in version.spec.reports}),
            historical_knowledge_attested=False,
            exact_reproduction=True,
            transitions=transitions,
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
        case = MappingCase.model_validate_json(args.case.read_bytes())
        host, output = args.host.resolve(), args.output.resolve()
        state = active(host)
        if any(output.is_relative_to(p) or p.is_relative_to(output) for p in (host, state)):
            raise ValueError("输出必须与活动环境独立")
        report = Report(output, case)
        configuration, _ = report.step("connection", lambda: read_connection(host, args.connection))
        report.step("acceptance", lambda: suite(report, case, configuration))
        print("角色验收通过：" + str(output / "report.json"))
        return 0
    except Exception as exc:  # noqa: BLE001 - credential-bearing exceptions must never be printed
        print("角色验收失败（" + type(exc).__name__ + "）；请检查报告中的失败阶段。")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
