"""Opt-in real Tushare acceptance; no account creation or writes to the source environment."""

import argparse
import json
import os
import secrets
from datetime import date, timedelta
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator
from sqlalchemy import create_engine, select

from asterion.contract_rules.mapping import MappingRequest
from asterion.contract_rules.public import RuleAccess, RuleSpec
from asterion.contract_rules.service import Rules
from asterion.contract_rules.settlement import SettlementConfirmation, SettlementRequest
from asterion.data.catalog import snapshots
from asterion.data.configuration import ConfigurationUpdate, configurations, validate
from asterion.data.connections import connection_settings, connections
from asterion.data.coverage import CoverageRequest
from asterion.data.ingestion import Observation
from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.public import CREDENTIAL_SCOPE, VersionAccess, VersionReader
from asterion.data.reference import ResolutionRequest
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.reference_store import ReferenceStore
from asterion.data.sync import Credentials, DataSync, collect
from asterion.distribution import strategy_catalog
from asterion.distribution_storage import data_storage, research_storage, rule_storage
from asterion.platform.secrets import secret_port
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs
from asterion.platform.task_port import task_port
from asterion.platform.tasks.service import Tasks
from asterion.research.engine import BacktestRequest, calculate
from asterion.research.packages import ResearchPackages
from asterion.research.service import Backtests
from asterion.runtime.desktop import runtime_settings
from asterion.runtime.environments import active
from asterion.trading_time.public import TimeVersion


class Case(BaseModel):
    trading_time: TimeVersion
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)
    schema_version: Literal[1]
    exchange: str
    symbol: str
    start: date
    end: date
    settlement_start: date
    multiplier: str
    tick_size: str
    fee_field: Literal["trading_fee", "trading_fee_rate"]
    fee_unit: Literal["yuan_per_lot", "ratio", "percent", "permille", "permyriad"]
    margin_unit: Literal["ratio", "percent"]
    interpretation: str = Field(min_length=1, max_length=1000)
    source: str = Field(min_length=1, max_length=1000)
    fast: int = Field(ge=1, le=30)
    slow: int = Field(ge=2, le=31)

    @model_validator(mode="after")
    def bounds(self):
        if not self.settlement_start < self.start <= self.end:
            raise ValueError("结算起点必须早于研究开始日期")
        if (self.end - self.settlement_start).days > 31 or self.fast >= self.slow:
            raise ValueError("验收范围最多 31 天且快均线必须短于慢均线")
        if (self.fee_field == "trading_fee") != (self.fee_unit == "yuan_per_lot"):
            raise ValueError("费用字段与单位不匹配")
        for kind in ("daily", "settlement"):
            Tushare().plan(self.request(kind))
        # Validate public numeric constraints before any network request; identity comes from data.
        from pydantic import TypeAdapter

        for field in ("multiplier", "tick_size"):
            TypeAdapter(RuleSpec.model_fields[field].rebuild_annotation()).validate_python(
                getattr(self, field)
            )
        return self

    def request(self, dataset):
        return SyncRequest(
            provider="tushare",
            command_id="acceptance-" + dataset,
            dataset=dataset,
            exchange=self.exchange,
            symbol=self.symbol if dataset in {"daily", "settlement"} else "",
            start=None
            if dataset == "contracts"
            else self.trading_time.spec.calendar[0].date
            if dataset == "calendar"
            else self.settlement_start
            if dataset == "settlement"
            else self.start,
            end=None if dataset == "contracts" else self.end,
        )


def read_connection(host: Path, identifier: str):
    """Use a read-only database transaction and the current encrypted configuration contract."""
    state = active(host)
    settings = runtime_settings(state, json.loads((state / "desktop.json").read_text()))
    engine = create_engine(
        settings.database_url, hide_parameters=True, connect_args={"connect_timeout": 5}
    )
    try:
        with engine.connect() as conn:
            conn.exec_driver_sql("SET TRANSACTION READ ONLY")
            owner = (
                conn.execute(select(connections).where(connections.c.id == identifier))
                .mappings()
                .one()
            )
            lifecycle = (
                conn.execute(
                    select(connection_settings).where(connection_settings.c.id == identifier)
                )
                .mappings()
                .first()
            )
            if owner["provider"] != "tushare" or (lifecycle and lifecycle["state"] != "enabled"):
                raise ValueError("必须选择启用的 Tushare 命名连接")
            row = (
                conn.execute(select(configurations).where(configurations.c.provider == identifier))
                .mappings()
                .one()
            )
            spec = Tushare.manifest.configuration
            if row["schema_version"] != spec.schema_version:
                raise ValueError("配置契约不受支持")
            credentials = Credentials(
                settings.data_root, secret_port(settings.token, CREDENTIAL_SCOPE)
            )
            values = credentials.read_configuration(
                row["snapshot_ref"], identifier, spec.schema_version, row["revision"]
            )
            return validate(spec, values), {
                "connection_id": identifier,
                "revision": row["revision"],
            }
    finally:
        engine.dispose()


class Report:
    def __init__(self, root: Path, case: BaseModel):
        root.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.root = root
        self.value = {
            "schema_version": 1,
            "status": "RUNNING",
            "case": case.model_dump(mode="json"),
            "steps": [],
        }
        self.write()

    def write(self):
        temporary = self.root / "report.tmp"
        temporary.write_bytes(canonical(self.value))
        temporary.replace(self.root / "report.json")

    def step(self, name, action):
        item = {"name": name, "status": "RUNNING"}
        self.value["steps"].append(item)
        self.write()
        try:
            result = action()
        except Exception as exc:
            # Exceptions can contain credentials or entire HTTP/SQL payloads: never serialize them.
            item.update(status="FAILED", error_type=type(exc).__name__)
            self.value["status"] = "FAILED"
            self.write()
            raise
        item["status"] = "PASSED"
        self.write()
        return result


def suite(report: Report, case: Case, configuration: dict):
    root = report.root
    key = secrets.token_urlsafe(32)
    with os.fdopen(
        os.open(root / "runtime.key", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w"
    ) as file:
        file.write(key)
    secrets_port = secret_port(key, CREDENTIAL_SCOPE)
    engine = create_engine("sqlite:///" + str(root / "acceptance.db"), hide_parameters=True)
    try:
        jobs.create(engine)
        tasks = Tasks(engine, lease_seconds=240)
        store = data_storage(engine)
        store.initialize(snapshots)
        sync = DataSync(
            store, task_port(tasks, frozenset({"data.sync", "data.import_csv"})), root, secrets_port
        )
        sync.configuration.apply(
            "tushare", ConfigurationUpdate(expected_revision=0, values={}, secrets=configuration)
        )
        ids = {}

        def publish(dataset, job=None):
            if job is None:
                from asterion.data.sync_admission import SyncSubmission, admit

                submitted = admit(
                    sync,
                    SyncSubmission.model_validate(
                        case.request(dataset).model_dump()
                        | {"contracts_version_id": ids["contracts"]}
                    ),
                )
                job = tasks.claim("acceptance")
                if not job or job["id"] != submitted["id"]:
                    raise RuntimeError("验收任务领取失败")
            try:
                content = collect(
                    job["payload"],
                    root,
                    secrets_port,
                    checkpoint=lambda index, value: sync.evidence.record(
                        job["id"], job["token"], index, Observation.model_validate(value)
                    ),
                )
                version = sync.publish(job["id"], job["token"], content)
            except Exception:
                tasks.fail(job["id"], job["token"], "验收采集或发布失败，见分阶段报告")
                raise
            ids[dataset] = version["id"]
            return {
                "id": version["id"],
                "rows": version["rows"],
                "checksum": version["manifest"]["checksum"],
            }

        report.value["versions"] = {}
        preparation = sync.preparations.submit(
            case.request("daily").model_copy(
                update={
                    "start": case.trading_time.spec.calendar[0].date,
                }
            )
        )
        if {task.type_id for task in preparation.tasks} != {
            "futures.calendar",
            "futures.contracts",
        }:
            raise ValueError("准备阶段不应提前提交日线")
        while job := tasks.claim("acceptance"):
            dataset = job["payload"]["request"]["dataset"]
            report.value["versions"][dataset] = report.step(
                dataset, lambda dataset=dataset, job=job: publish(dataset, job)
            )
        prepared = sync.preparations.get(preparation.id)
        if (
            prepared.daily_state != "SUBMITTED"
            or prepared.identity is None
            or set(ids) != {"calendar", "contracts", "daily"}
        ):
            raise ValueError("准备批次未通过固定身份准入")
        report.value["preparation"] = {
            "id": prepared.id,
            "daily_state": prepared.daily_state,
            "catalog_id": prepared.identity.catalog_id,
            "contracts_version_id": prepared.identity.catalog.inputs[0].version_id,
        }
        report.value["versions"]["settlement"] = report.step(
            "settlement", lambda: publish("settlement")
        )

        def publish_identity():
            catalog = source_catalog(
                sync.library.preview,
                SourceCatalogRequest(version_id=ids["contracts"], symbols=[case.symbol]),
            )
            release = ReferenceStore(store).publish(catalog)
            if release.id != prepared.identity.catalog_id:
                raise ValueError("准备批次与发布目录身份不一致")
            resolution = catalog.resolve(
                ResolutionRequest(
                    source="tushare",
                    symbol=case.symbol,
                    trading_day=case.start,
                    information_at=max(c.provenance.available_at for c in catalog.contracts),
                )
            )
            if not resolution.contract.listed_on <= case.end <= resolution.contract.last_trade_on:
                raise ValueError("验收区间超出实际合约生命周期")
            (root / "reference-catalog.json").write_bytes(
                canonical(release.model_dump(mode="json"))
            )
            return {
                "release_id": release.id,
                "contract_id": resolution.contract.id,
                "input_version_id": ids["contracts"],
                "historical_knowledge_attested": False,
            }

        report.value["identity"] = report.step("identity_catalog", publish_identity)
        report.write()
        coverage = report.step(
            "coverage",
            lambda: sync.coverage.check(
                ids["daily"],
                CoverageRequest(
                    start=case.start,
                    end=case.end,
                    calendar_version_id=ids["calendar"],
                    contracts_version_id=ids["contracts"],
                ),
            ),
        )
        coverage = coverage.model_dump(mode="json") if hasattr(coverage, "model_dump") else coverage
        if coverage["identity"]["catalog_id"] != report.value["identity"]["release_id"]:
            raise ValueError("覆盖报告与发布目录身份不一致")
        reader = VersionReader(store, root)
        calendar_rows = {
            r["date"]: bool(r["is_open"]) for r in reader.read(ids["calendar"], limit=5000)["rows"]
        }
        if any(
            calendar_rows.get(d.date.isoformat()) != d.is_open
            for d in case.trading_time.spec.calendar
        ):
            raise ValueError("时间规则日历与真实固定日历不一致")
        access = VersionAccess(reader.read, reader.coverage)
        rules = Rules(rule_storage(engine), access)

        def build_rules():
            if coverage["status"] != "COVERED":
                raise ValueError("真实数据覆盖未通过")
            metadata = rules.mapping.preview(
                MappingRequest(
                    version_id=ids["contracts"], contract_id=report.value["identity"]["contract_id"]
                )
            )
            bars = [
                row
                for row in sync.library.preview(ids["daily"])["rows"]
                if case.start.isoformat() <= row["trading_day"] <= case.end.isoformat()
            ]
            rows = sync.library.preview(ids["settlement"])["rows"]
            if len(bars) <= case.slow:
                raise ValueError("行情不足以验证信号与下一日成交")
            periods = []
            for i, bar in enumerate(bars):
                start = case.start if i == 0 else date.fromisoformat(bar["trading_day"])
                end = (
                    date.fromisoformat(bars[i + 1]["trading_day"]) - timedelta(days=1)
                    if i + 1 < len(bars)
                    else case.end
                )
                previous = [r for r in rows if r["trading_day"] < str(start)]
                if not previous:
                    raise ValueError("缺少生效日期之前的结算依据")
                day = max(r["trading_day"] for r in previous)
                evidence = rules.settlement.preview(
                    SettlementRequest(
                        version_id=ids["settlement"],
                        contract_id=report.value["identity"]["contract_id"],
                        trading_day=day,
                    )
                )
                periods.append(
                    rules.settlement.confirm(
                        SettlementConfirmation(
                            evidence=evidence,
                            start=start,
                            end=end,
                            fee_field=case.fee_field,
                            fee_unit=case.fee_unit,
                            margin_unit=case.margin_unit,
                            fee_scope="long_open_and_non_today_close",
                            availability_assumption="after_source_day",
                            interpretation=case.interpretation,
                        )
                    )
                )
            return rules.save(
                RuleSpec(
                    contract=metadata.contract,
                    trading_time=case.trading_time,
                    title="真实数据技术验收",
                    source=case.source,
                    multiplier=case.multiplier,
                    tick_size=case.tick_size,
                    basis=metadata.basis,
                    periods=periods,
                )
            )

        rule = report.step("rules", build_rules)
        research = Backtests(
            research_storage(engine),
            task_port(tasks, frozenset({"research.backtest"})),
            access,
            RuleAccess(rules.read),
            strategy_catalog(),
        )

        def compute(run):
            job = tasks.claim("acceptance")
            if not job or job["id"] != run["id"]:
                raise RuntimeError("研究任务领取失败")
            try:
                output = canonical(calculate(job["payload"], strategy_catalog()))
                research.publish(job["id"], job["token"], output)
                return output
            except Exception:
                tasks.fail(job["id"], job["token"], "验收计算或发布失败")
                raise

        def run_backtest():
            run = research.submit(
                BacktestRequest(
                    command_id="acceptance-research",
                    version_id=ids["daily"],
                    start=case.start,
                    end=case.end,
                    strategy=next(
                        s.identity
                        for s in strategy_catalog().list()
                        if s.identity.id == "builtin.sma-long"
                    ),
                    parameters={"fast": case.fast, "slow": case.slow},
                    capital="100000",
                    rules=rule,
                    slippage_ticks=1,
                    coverage_report_id=coverage["id"],
                    coverage_policy="require_complete",
                    assumption="historical-close-unverified-calendar",
                )
            )
            return run, compute(run)

        run, output = report.step("backtest", run_backtest)

        def rerun():
            if compute(research.rerun(run["id"], "acceptance-rerun")) != output:
                raise ValueError("重跑结果不一致")

        report.step("rerun", rerun)
        packages = ResearchPackages(research)
        package = report.step("export", lambda: packages.export(run["id"], True))
        (root / "research.asterion.json").write_bytes(canonical(package))

        def unavailable(*args, **kwargs):
            raise AssertionError("离线复现禁止读取来源或规则目录")

        def replay():
            research.versions = VersionAccess(unavailable, unavailable)
            research.rules = RuleAccess(unavailable)
            imported = packages.receive("acceptance", package)
            replayed = packages.replay("acceptance", imported["id"], "acceptance-offline")
            if (
                compute(replayed) != output
                or not research.get(replayed["id"])["result"]["reproduction_matches"]
            ):
                raise ValueError("离线复现结果不一致")

        report.step("offline_replay", replay)
        report.value.update(
            status="PASSED",
            coverage=coverage["status"],
            rule_id=rule.id,
            run_id=run["id"],
            exact_reproduction=True,
            summary=json.loads(output)["summary"],
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
    parser.add_argument("--connection", required=True, help="已有、启用的 Tushare 命名连接 ID")
    parser.add_argument("--case", type=Path, required=True, help="明确研究假设的验收 JSON")
    parser.add_argument("--output", type=Path, required=True, help="新的独立输出目录；拒绝覆盖")
    args = parser.parse_args()
    try:
        case = Case.model_validate_json(args.case.read_bytes())
        host, output = args.host.resolve(), args.output.resolve()
        state = active(host)
        if any(output.is_relative_to(p) or p.is_relative_to(output) for p in (host, state)):
            raise ValueError("输出必须与本机环境独立")
        report = Report(output, case)
        configuration, source = report.step(
            "connection", lambda: read_connection(host, args.connection)
        )
        report.value["source"] = source
        report.step("acceptance", lambda: suite(report, case, configuration))
        print("验收通过：" + str(output / "report.json"))
        return 0
    except Exception as exc:  # noqa: BLE001 - never expose credential-bearing exceptions
        print(
            "验收失败（"
            + type(exc).__name__
            + "）；若已创建输出目录，请查看 report.json 的失败阶段。"
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
