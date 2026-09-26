"""Portable research evidence. Packages contain data, never executable strategies."""

import base64
import csv
import hashlib
import io
import json
import zipfile
from datetime import date
from decimal import Decimal, InvalidOperation
from typing import Literal

from asterion_bindings.execution import validate_prices
from pydantic import BaseModel, ConfigDict, Field, model_validator
from sqlalchemy import JSON, Column, String, Table, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.data.public import coverage_contracts, normalize_coverage_report
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.research.data_input import selected_input
from asterion.research.engine import ENGINE, calculate
from asterion.research.service import KIND, LIMIT, WARNINGS, validate_input, validate_time_calendar
from asterion.research.strategies import StrategyUnavailable

LIMIT_BYTES = 8_000_000
packages = Table(
    "research_packages",
    metadata,
    Column("owner", String, primary_key=True),
    Column("id", String, primary_key=True),
    Column("package", JSON, nullable=False),
)


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


class PortableBar(BaseModel):
    model_config = ConfigDict(extra="forbid")
    trading_day: date
    contract_id: str = Field(min_length=1, max_length=100)
    contract: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}$")
    open: str = Field(max_length=64)
    high: str = Field(max_length=64)
    low: str = Field(max_length=64)
    close: str = Field(max_length=64)
    settle: str = Field(max_length=64)

    @model_validator(mode="after")
    def prices(self):
        validate_prices(
            {key: getattr(self, key) for key in ("open", "high", "low", "close", "settle")}
        )
        return self


class VersionReference(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1, max_length=100)
    dataset_id: str = Field(min_length=1, max_length=100)
    source: str = Field(max_length=100)
    type_id: Literal["futures.daily"] = "futures.daily"
    checksum: str = Field(pattern=r"^[a-f0-9]{64}$")


class PackageData(BaseModel):
    model_config = ConfigDict(extra="forbid")
    engine: str = Field(max_length=100)
    origin_run_id: str = Field(max_length=100)
    origin_input_checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    request: dict
    version: VersionReference
    coverage: dict | None
    bars_checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    output_checksum: str = Field(pattern=r"^[a-f0-9]{64}$")
    output: dict
    bars: list[PortableBar] | None = Field(default=None, max_length=LIMIT)


class Package(BaseModel):
    model_config = ConfigDict(extra="forbid")
    format: Literal["asterion.research"]
    schema_version: Literal[1]
    content: PackageData
    checksum: str = Field(pattern=r"^[a-f0-9]{64}$")


def portable(rows):
    return [
        PortableBar.model_validate(
            {
                k: str(row[k])
                for k in (
                    "trading_day",
                    "contract",
                    "contract_id",
                    "open",
                    "high",
                    "low",
                    "close",
                    "settle",
                )
            }
        ).model_dump(mode="json")
        for row in rows
    ]


def csv_text(rows, fields):
    stream = io.StringIO(newline="")
    writer = csv.writer(stream)
    writer.writerow(fields)
    for row in rows:
        cells = []
        for field in fields:
            text = str(row.get(field, "") if row.get(field) is not None else "")
            try:
                numeric = Decimal(text).is_finite()
            except InvalidOperation:
                numeric = False
            if not numeric and (
                text.lstrip().startswith(("=", "+", "-", "@"))
                or text.startswith(("\t", "\r", "\n"))
            ):
                text = "'" + text
            cells.append(text)
        writer.writerow(cells)
    return "\ufeff" + stream.getvalue()


class ResearchPackages:
    def __init__(self, service):
        self.service, self.engine = service, service.engine
        self.engine.initialize(packages)

    def export(self, run_id, include_data=False):
        detail = self.service.get(run_id)
        if detail["state"] != "SUCCEEDED" or not detail["output"]:
            raise ValueError("仅已完成的研究运行可导出")
        with self.engine.connect() as conn:
            payload = conn.execute(select(jobs.c.payload).where(jobs.c.id == run_id)).scalar_one()
        if (
            digest({k: v for k, v in payload.items() if k != "input_checksum"})
            != payload["input_checksum"]
        ):
            raise ValueError("原运行固定输入校验失败")
        rows = portable(payload["bars"])
        version = payload["version"]
        content = {
            "engine": payload["engine"],
            "origin_run_id": run_id,
            "origin_input_checksum": payload["input_checksum"],
            "request": payload["request"],
            "version": {
                "id": version["id"],
                "dataset_id": version["dataset_id"],
                "source": version["manifest"]["source"],
                "type_id": "futures.daily",
                "checksum": version["manifest"]["checksum"],
            },
            "coverage": payload["coverage"],
            "bars_checksum": digest(rows),
            "output_checksum": digest(detail["output"]),
            "output": detail["output"],
            "bars": rows if include_data else None,
        }
        envelope = {
            "format": "asterion.research",
            "schema_version": 1,
            "content": content,
            "checksum": digest(content),
        }
        if len(canonical(envelope)) > LIMIT_BYTES:
            raise ValueError("复现包超过 8 MB，请选择不附带行情")
        return envelope

    def result_archive(self, run_id):
        detail = self.service.get(run_id)
        output = detail["output"]
        if detail["state"] != "SUCCEEDED" or output is None:
            raise ValueError("仅已完成的研究运行可导出")
        files = {
            "rules.json": json.dumps(detail["request"]["rules"], ensure_ascii=False, indent=2),
            "equity.csv": csv_text(output["curve"], ["day", "contract_id", "equity", "drawdown"]),
            "settlements.csv": csv_text(
                output["curve"],
                [
                    "day",
                    "contract_id",
                    "session_open",
                    "session_close",
                    "time_version",
                    "settle",
                    "opening_balance",
                    "settlement_pnl",
                    "fees",
                    "balance",
                    "position",
                    "margin",
                    "free_cash",
                    "close_pnl",
                    "close_equity",
                ],
            ),
            "positions.csv": csv_text(
                output["curve"], ["day", "contract_id", "position", "margin", "free_cash"]
            ),
            "fills.csv": csv_text(
                output["fills"],
                [
                    "day",
                    "contract_id",
                    "time",
                    "side",
                    "lots",
                    "price",
                    "fee",
                    "fee_mode",
                    "fee_rate",
                    "rule_version",
                    "rule_start",
                    "reason",
                ],
            ),
            "events.csv": csv_text(output["events"], ["day", "contract_id", "reason"]),
            "summary.csv": csv_text([output["summary"]], list(output["summary"])),
        }
        # JSON code blocks keep user-supplied parameter text out of report markup.
        params = json.dumps(detail["request"], ensure_ascii=False, indent=2).replace("`", "\\u0060")
        summary = json.dumps(output["summary"], ensure_ascii=False, indent=2)
        files["report.md"] = (
            "# 星枢 · 日线回测报告\n\n"
            f"运行：{run_id}\n\n固定输入 SHA-256：{detail['input_checksum']}\n\n"
            "## 指标\n\n```json\n" + summary + "\n```\n\n"
            "金额以元计；收益率与回撤为比例。主权益为结算余额，回撤为每日结算回撤；settlements.csv 另列收盘估值差额与权益。"
            "期末不强制平仓，未计后续平仓费用。\n\n"
            "## 固定参数\n\n```json\n" + params + "\n```\n\n"
            "## 模型边界\n\n单合约日线做多/空仓模型；下一根日线开盘撮合。"
            "历史收盘可用是假设，不证明当时可得性。固定滑点和费用不代表真实成交。"
            "覆盖报告仅相对于固定依据版本，导入包中的依据为提供者声明。\n"
        )
        if detail.get("coverage"):
            files["coverage.json"] = json.dumps(detail["coverage"], ensure_ascii=False, indent=2)
        content = io.BytesIO()
        with zipfile.ZipFile(content, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, text in files.items():
                archive.writestr(name, text.encode("utf-8"))
        return {
            "filename": f"research-{run_id}.zip",
            "content_base64": base64.b64encode(content.getvalue()).decode(),
        }

    def receive(self, owner, value):
        if (
            not isinstance(value, dict)
            or value.get("format") != "asterion.research"
            or value.get("schema_version") != 1
        ):
            raise ValueError("不支持此复现包格式或版本")
        if value.get("checksum") != digest(value.get("content")):
            raise ValueError("复现包校验和不一致")
        # Preserve exact JSON; never accept a model normalization as checksum verification.
        try:
            Package.model_validate(value)
        except ValueError:
            raise ValueError("复现包结构无效或超出支持边界") from None
        ident = digest(value)
        insert = pg_insert if self.engine.dialect.name == "postgresql" else sqlite_insert
        with self.engine.begin() as conn:
            conn.execute(
                insert(packages)
                .values(owner=owner, id=ident, package=value)
                .on_conflict_do_nothing(index_elements=["owner", "id"])
            )
        return self.inspect(owner, ident)

    def _read(self, owner, ident):
        with self.engine.connect() as conn:
            value = conn.execute(
                select(packages.c.package).where(packages.c.owner == owner, packages.c.id == ident)
            ).scalar_one_or_none()
        if value is None:
            raise KeyError(ident)
        if digest(value) != ident or digest(value["content"]) != value["checksum"]:
            raise ValueError("已导入复现包校验失败")
        return value["content"]

    def _resolve(self, content):
        from asterion.research.engine import frozen_request

        config = frozen_request(content["request"])
        self.service.strategies.resolve(config.strategy).parameters(config.parameters)
        if config.version_id != content["version"]["id"]:
            raise ValueError("参数与版本引用不一致")
        if config.coverage_policy == "allow_incomplete" and not config.coverage_note.strip():
            raise ValueError("探索模式必须说明原因")
        coverage = content["coverage"]
        if config.coverage_report_id and not coverage:
            raise ValueError("缺少参数引用的覆盖报告")
        if coverage:
            coverage = normalize_coverage_report(coverage)
            if (
                coverage["daily_version_id"] != config.version_id
                or coverage["start"] != config.start.isoformat()
                or coverage["end"] != config.end.isoformat()
                or coverage["id"] != config.coverage_report_id
            ):
                raise ValueError("覆盖报告与固定参数不一致")
        if config.coverage_policy == "require_complete" and (
            not coverage or coverage["status"] != "COVERED"
        ):
            raise ValueError("严格模式缺少通过的固定覆盖报告")
        if coverage and coverage["status"] == "CONFLICT":
            raise ValueError("包内覆盖依据存在冲突")
        validate_time_calendar(config, coverage)
        rows = content["bars"]
        resolution = "embedded"
        if rows is None:
            # The original frozen run is itself an immutable local data snapshot.
            with self.engine.connect() as conn:
                original = conn.execute(
                    select(jobs.c.payload).where(
                        jobs.c.id == content["origin_run_id"], jobs.c.kind == KIND
                    )
                ).scalar_one_or_none()
            if original and original.get("input_checksum") == content["origin_input_checksum"]:
                rows = portable(original["bars"])
                resolution = "local_run"
            else:
                try:
                    preview = selected_input(self.service.versions, config, LIMIT)
                    if preview["version"]["manifest"]["checksum"] != content["version"]["checksum"]:
                        raise ValueError("本机版本与包内引用不一致")
                    selected = [
                        r
                        for r in preview["rows"]
                        if config.start.isoformat()
                        <= str(r["trading_day"])
                        <= config.end.isoformat()
                    ]
                    actuals = coverage_contracts(
                        coverage, [date.fromisoformat(str(r["trading_day"])) for r in selected]
                    )
                    rows = portable(
                        [
                            r | {"contract_id": actual["id"]}
                            for r, actual in zip(selected, actuals, strict=True)
                        ]
                    )
                    resolution = "local_version"
                except KeyError:
                    return None, "MISSING_DATA", "本机缺少固定行情版本，请导入附带行情的复现包"
        rows = portable(rows)
        if digest(rows) != content["bars_checksum"]:
            raise ValueError("回测行情校验和不一致")
        days = [row["trading_day"] for row in rows]
        if (
            not self.service.strategies.resolve(config.strategy).required_bars(config.parameters)
            < len(rows)
            <= LIMIT
            or days != sorted(set(days))
            or len({r["contract"] for r in rows}) != 1
        ):
            raise ValueError("行情必须是有序、无重复且满足预热的单合约日线")
        if days[0] < config.start.isoformat() or days[-1] > config.end.isoformat():
            raise ValueError("行情超出固定日期范围")
        if coverage and {d["date"] for d in coverage["days"] if d["has_data"]} != set(days):
            raise ValueError("覆盖报告的有数据日期与行情不一致")
        ref = content["version"]
        payload = {
            "engine": ENGINE,
            "request": config.model_dump(mode="json", exclude={"command_id"}),
            "bars": rows,
            "version": {
                "id": ref["id"],
                "dataset_id": ref["dataset_id"],
                "rows": len(rows),
                "manifest": {
                    "source": ref["source"],
                    "checksum": ref["checksum"],
                    "type": {"id": "futures.daily"},
                    "layer": "STANDARD",
                    "first": days[0],
                    "last": days[-1],
                    "scope": {"contract": rows[0]["contract"]},
                },
            },
            "coverage": coverage,
            "warnings": [*WARNINGS, "此运行为导入复现；包内来源和覆盖依据未经独立认证。"],
            "reproduction": {
                "origin_run_id": content["origin_run_id"],
                "origin_input_checksum": content["origin_input_checksum"],
                "expected_output_checksum": content["output_checksum"],
            },
        }
        payload["input_checksum"] = digest(payload)
        validate_input(payload)
        if (
            digest(content["output"]) != content["output_checksum"]
            or digest(calculate(payload, self.service.strategies, self.service.execution))
            != content["output_checksum"]
        ):
            raise ValueError("包内结果与固定输入重新计算的结果不一致")
        return payload, "READY", resolution

    def inspect(self, owner, ident):
        content = self._read(owner, ident)
        status, detail = "UNSUPPORTED", "当前不支持包内引擎或策略版本"
        if content["engine"] == ENGINE:
            try:
                _, status, detail = self._resolve(content)
            except StrategyUnavailable as exc:
                status, detail = "UNSUPPORTED", str(exc)
            except (ValueError, TypeError, KeyError, ArithmeticError) as exc:
                status, detail = (
                    "INVALID",
                    "行情、参数、覆盖报告或结果核验失败；请检查复现包及本机固定版本",
                )
                if str(exc) == "数据文件缺失":
                    status, detail = "MISSING_DATA", "本机固定版本文件缺失，请导入附带行情的复现包"
        return {
            "id": ident,
            "status": status,
            "detail": detail,
            "engine": content["engine"],
            "strategy": content["request"].get("strategy"),
            "version_id": content["version"]["id"],
            "origin_run_id": content["origin_run_id"],
            "includes_data": content["bars"] is not None,
            "output_checksum": content["output_checksum"],
            "can_replay": status == "READY",
        }

    def replay(self, owner, ident, command_id):
        content = self._read(owner, ident)
        if content["engine"] != ENGINE:
            raise ValueError("不支持包内引擎或策略版本")
        try:
            payload, status, detail = self._resolve(content)
        except (ValueError, TypeError, KeyError, ArithmeticError):
            raise ValueError("复现输入核验失败，请重新核验复现包") from None
        if status != "READY" or payload is None:
            raise ValueError(detail)
        return self.service.tasks.submit(command_id, KIND, payload)


def parse_package(raw: bytes):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ValueError("复现包包含重复字段")
            result[key] = value
        return result

    try:
        return json.loads(
            raw.decode("utf-8-sig"),
            object_pairs_hook=pairs,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError("非有限数值")),
        )
    except (UnicodeError, ValueError, RecursionError):
        raise ValueError("复现包不是有效且无重复字段的 JSON") from None
