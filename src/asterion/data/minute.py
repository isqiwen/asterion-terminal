"""Provider minute semantics bound to explicit immutable trading-time evidence."""

from datetime import date, datetime
from typing import Literal

from asterion_bindings import data_minute
from asterion_bindings.calendar import TimeVersion
from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator
from sqlalchemy import select

from asterion.data.providers.public import MinuteFrequency, SourceWindow

TYPE = "futures.minute"


class MinuteContext(BaseModel):
    """The fixed basis of a minute task. Semantics are the Rust data store's;
    this model is only the request shape of the internal process."""

    model_config = ConfigDict(extra="forbid", frozen=True)
    frequency: MinuteFrequency
    trading_time: TimeVersion
    timestamp_semantics: Literal["bar_start", "bar_end"]
    semantics_source: str = Field(min_length=1, max_length=2000)

    @model_validator(mode="before")
    @classmethod
    def validated(cls, value):
        if isinstance(value, dict):
            try:
                return data_minute.context(TypeAdapter(dict).dump_python(value, mode="json"))
            except ValueError as error:
                raise ValueError(str(error)) from None
        return value

    def label_stamps(self, contract: str, day: date) -> list[datetime]:
        stamps = data_minute.labels(self.model_dump(mode="json"), contract, day.isoformat())
        return [datetime.fromisoformat(stamp) for stamp in stamps]

    def window(self, contract: str, day: date) -> SourceWindow:
        return SourceWindow.model_validate(
            data_minute.window(self.model_dump(mode="json"), contract, day.isoformat())
        )


def bind_rows(rows, context: MinuteContext, observed: str):
    """Check rows against the fixed sessions and set their availability time."""
    bound = data_minute.bind(context.model_dump(mode="json"), rows, observed)
    for row, value in zip(rows, bound, strict=True):
        row.update(value)
    return rows


class MinuteCoverage(BaseModel):
    version_id: str
    checksum: str
    contract_id: str
    trading_day: date
    trading_time_id: str
    frequency: MinuteFrequency
    status: Literal["COVERED", "GAPS", "UNVERIFIED"]
    expected: int | None
    present: int
    missing: list[str] | None
    note: str = "缺少记录不自动补零；须核对来源是否省略无成交分钟。"


def coverage(sync, identifier: str, day: date):
    from asterion_bindings.catalog import SourceIdentity

    from asterion.data.library import versions
    from asterion.data.partitions import read_partition, version_partitions

    with sync.engine.connect() as conn:
        record = (
            conn.execute(select(versions).where(versions.c.id == identifier)).mappings().first()
        )
    if record is None:
        raise KeyError(identifier)
    manifest = record["manifest"]
    if manifest["type"]["id"] != TYPE or manifest["layer"] != "STANDARD":
        raise ValueError("请选择标准分钟版本")
    context = MinuteContext.model_validate(manifest["minute_context"])
    if (
        manifest["scope"].get("frequency") != context.frequency
        or manifest["type"]["frequency"] != context.frequency
        or manifest["scope"].get("trading_time_id") != context.trading_time.id
        or manifest["scope"].get("timestamp_semantics") != context.timestamp_semantics
    ):
        raise ValueError("分钟范围与固定时间依据不一致")
    identity = SourceIdentity.model_validate(manifest["contract_identity"])
    actual = identity.resolve(day)
    contract = manifest["scope"]["exchange"] + "." + manifest["scope"]["symbol"].split(".")[0]
    expected = {s.isoformat() for s in context.label_stamps(contract, day)}
    if not expected:
        raise ValueError("所选日期没有可核对的交易时段")
    artifacts = sync.library.artifacts
    rows = [
        row
        for part in version_partitions(artifacts, manifest)
        if part.key == day.isoformat()
        for row in read_partition(artifacts, part)
    ]
    sync.library.types.get(TYPE).validate(rows)
    if rows:
        identity.validate_rows(rows, manifest["scope"]["exchange"])
        for row in rows:
            bind_rows([row], context, row["available_at"])
            if row["trading_day"] != day.isoformat():
                raise ValueError("分钟分区交易日不一致")
    present = {row["event_time"] for row in rows}
    missing = sorted(expected - present)
    return MinuteCoverage(
        version_id=identifier,
        checksum=manifest["checksum"],
        contract_id=actual.id,
        trading_day=day,
        trading_time_id=context.trading_time.id,
        frequency=context.frequency,
        status=("GAPS" if missing else "COVERED") if context.frequency == "1m" else "UNVERIFIED",
        expected=len(expected) if context.frequency == "1m" else None,
        present=len(present),
        missing=missing if context.frequency == "1m" else None,
        note=(
            "缺少记录不自动补零；须核对来源是否省略无成交分钟。"
            if context.frequency == "1m"
            else "已校验记录时间归属；供应商跨休市及不足周期的分段规则尚未核验，不能判定完整性。"
        ),
    )
