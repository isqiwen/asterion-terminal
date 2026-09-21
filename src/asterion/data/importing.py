"""Strict file adapters into the same data types used by provider plugins."""

import csv
import io
import re
from datetime import UTC, date, datetime
from decimal import Decimal, InvalidOperation

import pyarrow as pa
import pyarrow.parquet as pq
from pydantic import BaseModel, ValidationError

from asterion.data.public import Bar, ImportOptions, encode_csv

BAR_FIELDS = list(Bar.model_fields)
DAILY_FIELDS = [
    "contract",
    "trading_day",
    "open",
    "high",
    "low",
    "close",
    "vol",
    "amount",
    "oi",
    "settle",
]


class ImportPreview(BaseModel):
    contract_ids: list[str]
    columns: list[str]
    fields: list[str]
    required: list[str]
    rows: list[dict]
    total: int
    valid: bool
    errors: list[str]


def parse(content):
    if len(content.encode("utf-8")) > 2_000_000:
        raise ValueError("CSV 超过 2 MB")
    reader = csv.DictReader(io.StringIO(content.lstrip("\ufeff")), strict=True)
    names = list(reader.fieldnames or [])
    if not names or len(names) != len(set(names)) or any(not name.strip() for name in names):
        raise ValueError("CSV 表头不能为空或重复")
    try:
        rows = list(reader)
    except csv.Error:
        raise ValueError("CSV 引号或记录格式错误") from None
    if not rows or any(None in row or any(v is None for v in row.values()) for row in rows):
        raise ValueError("CSV 为空或数据列数与表头不一致")
    return names, rows


def mapped(content: str, options: ImportOptions):
    from asterion.data.types import builtin_types

    names, rows = parse(content)
    fields = DAILY_FIELDS if options.type_id == "futures.daily" else BAR_FIELDS
    required = (
        fields[:7]
        if options.type_id == "futures.daily"
        else [f for f in fields if f != "trading_day"]
    )
    mapping = options.column_mapping or {key: key for key in fields if key in names}
    if set(mapping) - set(fields) or set(mapping.values()) - set(names):
        raise ValueError("字段映射包含不存在的字段或列名")
    if len(set(mapping.values())) != len(mapping):
        raise ValueError("同一文件列不能映射到多个标准字段")
    if not set(required) <= set(mapping):
        raise ValueError(
            "缺少必填字段映射：" + ", ".join(key for key in required if key not in mapping)
        )
    output = [{key: row[column].strip() for key, column in mapping.items()} for row in rows]
    if options.type_id == "futures.daily":
        for row in output:
            row["trading_day"] = date.fromisoformat(row["trading_day"]).isoformat()
            if date.fromisoformat(row["trading_day"]) > datetime.now(UTC).date():
                raise ValueError("历史日线不能包含未来日期")
            for field in ("open", "high", "low", "close"):
                try:
                    value = Decimal(row[field])
                except InvalidOperation:
                    raise ValueError("价格必须为有效数字") from None
                if not value.is_finite() or value <= 0:
                    raise ValueError("当前日线图表要求价格为有限正数")
            if not re.fullmatch(
                r"(?:(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}|SIM\.DEMO001)",
                row["contract"],
            ):
                raise ValueError("标准合约格式应为交易所.合约代码")
            row["exchange"], row["symbol"] = row["contract"].split(".")
            for field in ("amount", "oi", "settle"):
                row[field] = row.get(field) or None
        if len({row["contract"] for row in output}) != 1:
            raise ValueError("日线文件请按单个合约导入，保证数据集范围明确")
    if options.trading_time:
        spec = options.trading_time.spec
        for row in output:
            if options.type_id == "futures.daily":
                spec.daily(row["contract"], date.fromisoformat(row["trading_day"]))
            else:
                assert options.timestamp_semantics is not None
                seconds = {"1m": 60, "5m": 300, "15m": 900, "30m": 1800, "1h": 3600}[
                    options.frequency
                ]
                stamp = datetime.fromisoformat(row["event_time"])
                resolved = spec.resolve(
                    row["contract"],
                    stamp,
                    "bar_end" if options.timestamp_semantics == "bar_end" else "event",
                )
                if "trading_day" not in row:
                    row["trading_day"] = resolved.trading_day.isoformat()
                spec.validate_bar(
                    row["contract"],
                    stamp,
                    date.fromisoformat(row["trading_day"]),
                    seconds,
                    options.timestamp_semantics,
                )
    options.identity.validate_rows(output)
    builtin_types().get(options.type_id).validate(output)
    key = builtin_types().get(options.type_id).manifest.primary_key
    output.sort(key=lambda row: tuple(row[field] for field in key))
    return output


def encode_import(payload):
    if "options" not in payload:
        raise ValueError("导入任务缺少当前数据规范，请重新提交")
    options = ImportOptions.model_validate(payload["options"])
    rows = mapped(payload["csv"], options)
    if options.type_id == "futures.bars":
        stream = io.StringIO()
        writer = csv.DictWriter(stream, fieldnames=BAR_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
        content, manifest = encode_csv(stream.getvalue())
        return content, manifest | {"frequency": options.frequency}
    for row in rows:
        Bar.model_validate(
            {
                "contract": row["contract"],
                "event_time": row["trading_day"] + "T00:00:00Z",
                "available_at": row["trading_day"] + "T00:00:00Z",
                "trading_day": row["trading_day"],
                **{key: row[key] for key in ("open", "high", "low", "close")},
                "volume": int(Decimal(row["vol"])),
            }
        )
    # Match provider standard storage (canonical scalar strings), not chart timestamps.
    for row in rows:
        for key in ("open", "high", "low", "close", "vol", "amount", "oi", "settle"):
            if row.get(key) is not None:
                row[key] = str(Decimal(row[key]))
    stream = pa.BufferOutputStream()
    pq.write_table(pa.Table.from_pylist(rows), stream)
    return stream.getvalue().to_pybytes(), {
        "rows": len(rows),
        "contracts": [rows[0]["contract"]],
        "frequency": "1d",
        "demo": rows[0]["exchange"] == "SIM",
        "start": rows[0]["trading_day"],
        "end": rows[-1]["trading_day"],
    }


def preview(content: str, options: ImportOptions):
    fields = DAILY_FIELDS if options.type_id == "futures.daily" else BAR_FIELDS
    names, _ = parse(content)
    try:
        rows = mapped(content, options)
        encode_import({"csv": content, "options": options.model_dump()})
    except (ValueError, ValidationError) as exc:
        return ImportPreview(
            contract_ids=[],
            columns=names,
            fields=fields,
            required=fields[:7]
            if options.type_id == "futures.daily"
            else [f for f in fields if f != "trading_day"],
            rows=[],
            total=0,
            valid=False,
            errors=[str(exc)],
        )
    return ImportPreview(
        contract_ids=sorted(
            {
                options.identity.resolve(row["contract"], date.fromisoformat(row["trading_day"])).id
                for row in rows
            }
        ),
        columns=names,
        fields=fields,
        required=fields[:7]
        if options.type_id == "futures.daily"
        else [f for f in fields if f != "trading_day"],
        rows=rows[:10],
        total=len(rows),
        valid=True,
        errors=[],
    )
