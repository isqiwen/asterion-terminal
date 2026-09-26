"""Freeze research inputs at submission; publish only under the current task lease."""

import hashlib
import json
from datetime import date
from decimal import Decimal

from asterion_bindings.execution import ExecutionFactory, settlement_price
from asterion_bindings.task_repository import Conflict
from sqlalchemy import JSON, Column, String, Table, select

from asterion.contract_rules.public import RuleAccess
from asterion.data.public import VersionAccess, coverage_contracts, normalize_coverage_report
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.research.data_input import selected_input
from asterion.research.engine import (
    ENGINE,
    BacktestRequest,
    calculate,
    frozen_request,
)
from asterion.research.strategies import StrategyCatalog

results = Table(
    "research_results",
    metadata,
    Column("job_id", String, primary_key=True),
    Column("checksum", String, nullable=False),
    Column("output", JSON, nullable=False),
)
KIND = "research.backtest"
LIMIT = 5000
WARNINGS = [
    "历史收盘可用假设：数据可能在事后采集或修订，不是历史时点可得数据回放。",
    "按所选版本的下一根日线开盘撮合；未核验交易日历完整性，缺失交易日可能改变结果。",
    "单合约做多/空仓；按日线结算价逐日结算，按固定规则版本计费；不模拟涨跌停、流动性及真实强平。",
    "日线开盘价解释为交易日首个连续交易时段开盘（可能在前一自然日夜间），需核对来源日线聚合口径。",
    "主权益与回撤按结算余额计算；另列收盘估值。期末不强制平仓，未计入后续平仓费用。",
]


def validate_time_calendar(request, coverage):
    if coverage is None:
        return
    calendar = {
        d.date.isoformat(): d.is_open for d in request.rules.spec.trading_time.spec.calendar
    }
    for day in coverage["days"]:
        if day["status"] in {"PRESENT", "GAP", "CLOSED"} and calendar.get(day["date"]) != (
            day["status"] != "CLOSED"
        ):
            raise ValueError("交易时间日历与固定覆盖依据冲突")


def validate_input(payload):
    if payload.get("engine") != ENGINE or "coverage" not in payload:
        raise ValueError("不支持此研究固定输入契约")
    request = frozen_request(payload["request"])
    if (
        hashlib.sha256(
            canonical({k: v for k, v in payload.items() if k != "input_checksum"})
        ).hexdigest()
        != payload["input_checksum"]
    ):
        raise ValueError("研究输入校验和不一致")
    for bar in payload["bars"]:
        settlement_price(bar.get("settle"))
        request.rules.spec.trading_time.spec.daily(
            bar["contract"], date.fromisoformat(bar["trading_day"])
        )
        request.rules.spec.at(date.fromisoformat(bar["trading_day"]))
    coverage = payload["coverage"]
    if coverage is None:
        raise ValueError("研究固定输入缺少合约身份目录")
    actual_ids = set()
    actuals = coverage_contracts(
        coverage, [date.fromisoformat(bar["trading_day"]) for bar in payload["bars"]]
    )
    for bar, actual in zip(payload["bars"], actuals, strict=True):
        timing = request.rules.spec.trading_time.spec
        if (
            bar.get("contract_id") != actual["id"]
            or actual["product_id"] != f"{timing.exchange}.{timing.product}"
        ):
            raise ValueError("研究行情与固定合约身份目录不一致")
        actual_ids.add(actual["id"])
    if len(actual_ids) != 1:
        raise ValueError("研究区间必须属于单个实际合约生命周期")
    request.rules.spec.cover(next(iter(actual_ids)), request.start, request.end)
    if coverage is not None:
        coverage = normalize_coverage_report(coverage)
        validate_time_calendar(request, coverage)
        if (
            coverage["id"] != request.coverage_report_id
            or coverage["daily_version_id"] != request.version_id
            or coverage["start"] != request.start.isoformat()
            or coverage["end"] != request.end.isoformat()
            or coverage["status"] == "CONFLICT"
        ):
            raise ValueError("研究固定覆盖依据不一致或存在冲突")
    elif request.coverage_report_id:
        raise ValueError("研究缺少固定覆盖报告")
    if request.coverage_policy == "require_complete" and (
        not coverage or coverage["status"] != "COVERED"
    ):
        raise ValueError("严格研究需要固定且通过的覆盖报告")


class Backtests:
    def __init__(
        self,
        engine,
        tasks,
        versions: VersionAccess,
        rules: RuleAccess,
        strategies: StrategyCatalog,
        execution: ExecutionFactory,
    ):
        self.engine, self.tasks = engine, tasks
        self.versions = versions
        self.rules = rules
        self.strategies = strategies
        self.execution = execution
        engine.initialize(results)

    def submit(self, body: BacktestRequest):
        strategy = self.strategies.resolve(body.strategy)
        strategy.parameters(body.parameters)
        request = body.model_dump(mode="json", exclude={"command_id"})
        with self.engine.connect() as conn:
            old = (
                conn.execute(select(jobs).where(jobs.c.command_id == body.command_id))
                .mappings()
                .first()
            )
        if old:
            if old["kind"] != KIND or old["payload"]["request"] != request:
                raise Conflict("command_id reused with different input")
            return dict(old)
        return self.tasks.submit(body.command_id, KIND, self.prepare(body))

    def prepare(self, body: BacktestRequest):
        strategy = self.strategies.resolve(body.strategy)
        strategy.parameters(body.parameters)
        request = body.model_dump(mode="json", exclude={"command_id"})
        try:
            preview = selected_input(self.versions, body, LIMIT)
        except KeyError:
            raise ValueError("找不到指定数据版本") from None
        version = preview["version"]
        manifest = version["manifest"]
        if manifest["type"]["id"] != "futures.daily" or manifest["layer"] != "STANDARD":
            raise ValueError("回测需要标准化日线数据版本")
        if manifest.get("demo"):
            raise ValueError("研究运行不接受演示数据")
        selected = []
        seen = set()
        for index, row in enumerate(preview["rows"]):
            day = date.fromisoformat(str(row["trading_day"]))
            if not body.start <= day <= body.end:
                continue
            contract = str(row["contract"])
            body.rules.spec.trading_time.spec.daily(contract, day)
            if day in seen:
                raise ValueError("所选日期包含多合约或重复日线")
            seen.add(day)
            prices = {key: Decimal(str(row[key])) for key in ("open", "high", "low", "close")}
            if any(not p.is_finite() or p <= 0 or p > Decimal("1e12") for p in prices.values()):
                raise ValueError("日线价格无效")
            if (
                not prices["low"]
                <= min(prices["open"], prices["close"])
                <= max(prices["open"], prices["close"])
                <= prices["high"]
            ):
                raise ValueError("日线价格范围无效")
            sources = preview["row_sources"]
            selected.append(
                {
                    "trading_day": day.isoformat(),
                    "contract": contract,
                    "settle": str(settlement_price(row.get("settle"))),
                    **{key: str(value) for key, value in prices.items()},
                    "provenance": sources[index]
                    if sources
                    else {"version_created_at": version["created_at"]},
                }
            )
        selected.sort(key=lambda row: row["trading_day"])
        if len(selected) <= strategy.required_bars(body.parameters):
            raise ValueError("所选区间需要多于策略预热周期的日线；预热包含在区间内")
        if len({row["contract"] for row in selected}) != 1:
            raise ValueError("第一版只支持单一实际合约")
        if self.rules.read(body.rules.id) != body.rules:
            raise ValueError("所选规则与已保存版本不一致")
        coverage = None
        if body.coverage_report_id:
            try:
                coverage = self.versions.coverage(body.coverage_report_id)
            except KeyError:
                raise ValueError("覆盖报告不存在，请重新核对") from None
            if (
                coverage["daily_version_id"] != body.version_id
                or coverage["start"] != body.start.isoformat()
                or coverage["end"] != body.end.isoformat()
            ):
                raise ValueError("覆盖报告与所选数据版本或日期范围不一致，请重新核对")
            if coverage["status"] == "CONFLICT":
                raise ValueError("行情与覆盖依据冲突，请先修复数据或核对依据")
            reported = {d["date"] for d in coverage["days"] if d["has_data"]}
            if reported != {bar["trading_day"] for bar in selected}:
                raise ValueError("覆盖报告记录与固定日线不一致，请检查数据完整性")
        if body.coverage_policy == "require_complete":
            if not coverage or coverage["status"] != "COVERED":
                raise ValueError(
                    "需要所选版本和区间的通过报告；请核对并补齐数据，或明确选择探索模式"
                )
        elif not body.coverage_note.strip():
            raise ValueError("探索模式必须填写接受未核验或不完整数据的原因")
        if coverage is None:
            raise ValueError("研究需要固定覆盖报告与可验证的合约身份目录；探索模式也不能省略")
        actual_ids = set()
        actuals = coverage_contracts(
            coverage, [date.fromisoformat(bar["trading_day"]) for bar in selected]
        )
        for bar, actual in zip(selected, actuals, strict=True):
            timing = body.rules.spec.trading_time.spec
            if actual["product_id"] != f"{timing.exchange}.{timing.product}":
                raise ValueError("合约身份目录与交易时间品种不一致")
            bar["contract_id"] = actual["id"]
            actual_ids.add(actual["id"])
        if len(actual_ids) != 1:
            raise ValueError("研究区间包含不同生命周期的实际合约")
        body.rules.spec.cover(next(iter(actual_ids)), body.start, body.end)
        validate_time_calendar(body, coverage)
        warnings = list(WARNINGS)
        if coverage:
            warnings[1] = (
                "已绑定固定覆盖报告："
                + coverage["status"]
                + "；仅相对于报告列出的日历和合约资料，不证明历史时点可得性。"
            )
        payload = {
            "coverage": coverage,
            "engine": ENGINE,
            "request": request,
            "bars": selected,
            "version": version,
            "warnings": warnings,
        }
        payload["input_checksum"] = hashlib.sha256(canonical(payload)).hexdigest()
        return payload

    def list(self):
        with self.engine.connect() as conn:
            rows = conn.execute(
                select(jobs)
                .where(jobs.c.kind == KIND)
                .order_by(jobs.c.created_at.desc())
                .limit(100)
            ).mappings()
            return [self.summary(row) for row in rows]

    @staticmethod
    def summary(row):
        payload = row["payload"]
        return {
            "id": row["id"],
            "state": row["state"],
            "created_at": row["created_at"],
            "error": row["error"],
            "request": payload["request"],
            "input_checksum": payload["input_checksum"],
            "engine": payload["engine"],
            "contract": payload["bars"][0]["contract"],
            "contract_id": payload["bars"][0]["contract_id"],
            "result": row["result"],
        }

    def get(self, run_id):
        with self.engine.connect() as conn:
            row = (
                conn.execute(select(jobs).where(jobs.c.id == run_id, jobs.c.kind == KIND))
                .mappings()
                .first()
            )
            if row is None:
                raise KeyError(run_id)
            validate_input(row["payload"])
            output = (
                conn.execute(select(results).where(results.c.job_id == run_id)).mappings().first()
            )
            if (
                output
                and hashlib.sha256(canonical(output["output"])).hexdigest() != output["checksum"]
            ):
                raise ValueError("研究结果校验失败")
            return self.summary(row) | {
                "output": output["output"] if output else None,
                "warnings": row["payload"]["warnings"],
                "version": row["payload"]["version"],
                "coverage": row["payload"].get("coverage"),
                "reproduction": row["payload"].get("reproduction"),
            }

    def rerun(self, run_id, command_id):
        with self.engine.connect() as conn:
            row = (
                conn.execute(select(jobs).where(jobs.c.id == run_id, jobs.c.kind == KIND))
                .mappings()
                .first()
            )
            if row is None:
                raise KeyError(run_id)
            validate_input(row["payload"])
            frozen = frozen_request(row["payload"]["request"])
            self.strategies.resolve(frozen.strategy).parameters(frozen.parameters)
        # The frozen input is immutable; release its read transaction before
        # opening the independent task submission transaction.
        return self.tasks.submit(command_id, KIND, row["payload"])

    def publish(self, job_id, token, content):
        checksum = hashlib.sha256(content).hexdigest()
        with self.engine.begin() as conn:
            row = (
                conn.execute(select(jobs).where(jobs.c.id == job_id).with_for_update())
                .mappings()
                .first()
            )
            if (
                row
                and row["kind"] == KIND
                and row["state"] == "SUCCEEDED"
                and row["token"] == token
            ):
                existing = (
                    conn.execute(select(results).where(results.c.job_id == job_id)).mappings().one()
                )
                if existing["checksum"] == checksum:
                    return row["result"]
                raise Conflict("已发布结果不可修改")
            row = self.tasks.require_lease(conn, job_id, token)
            if row["kind"] != KIND:
                raise Conflict("不是研究任务")
            validate_input(row["payload"])
            expected = canonical(calculate(row["payload"], self.strategies, self.execution))
            if content != expected:
                raise ValueError("回测结果与固定输入不一致")
            # Recheck expiry after computation; publication and task completion are atomic.
            self.tasks.require_lease(conn, job_id, token)
            reproduction = row["payload"].get("reproduction")
            if reproduction and checksum != reproduction["expected_output_checksum"]:
                raise ValueError("复现结果校验和与原运行不一致")
            output = json.loads(content)
            result = {"run_id": job_id, "checksum": checksum, **output["summary"]}
            if reproduction:
                result["reproduction_matches"] = True
            conn.execute(results.insert().values(job_id=job_id, checksum=checksum, output=output))
            self.tasks.complete(conn, job_id, token, result)
            return result
