"""Immutable, product-specific exchange time contract. No weekday heuristics."""

import hashlib
import re
from datetime import date, datetime, time, timedelta
from functools import cached_property
from itertools import pairwise
from typing import Literal
from zoneinfo import ZoneInfo

from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

from asterion.platform.serialization import canonical

ZONE = ZoneInfo("Asia/Shanghai")


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class CalendarDay(Strict):
    date: date
    is_open: bool = Field(strict=True)
    # Whether the evening beginning on this NATURAL date is open.
    night_open: bool = Field(strict=True)

    @model_validator(mode="after")
    def valid(self):
        if self.night_open and not self.is_open:
            raise ValueError("夜盘起始自然日必须是日历开市日")
        return self


class Slot(Strict):
    start: str = Field(pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$")
    end: str = Field(pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$")
    end_offset: Literal[0, 1]
    phase: Literal["auction", "continuous"]

    @model_validator(mode="after")
    def valid(self):
        if self.end_offset == 0 and self.end <= self.start:
            raise ValueError("时段结束必须晚于开始")
        if self.end_offset == 1 and self.end > self.start:
            raise ValueError("单个交易时段不得超过 24 小时")
        return self


class Period(Strict):
    start: date
    end: date
    source: str = Field(min_length=1, max_length=2000)
    day: list[Slot] = Field(min_length=1, max_length=20)
    night: list[Slot] = Field(max_length=20)

    @model_validator(mode="after")
    def valid(self):
        if self.end < self.start or any(s.end_offset for s in self.day):
            raise ValueError("日盘日期或时段无效")
        if not any(s.phase == "continuous" for s in self.day):
            raise ValueError("日盘须包含连续交易时段")
        return self


class ExceptionDay(Strict):
    trading_day: date
    source: str = Field(min_length=1, max_length=2000)
    day: list[Slot] = Field(max_length=20)
    night: list[Slot] = Field(max_length=20)


class TimeSpec(Strict):
    schema_version: Literal[1]
    exchange: Literal["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]
    product: str = Field(pattern=r"^[A-Z]+$", max_length=20)
    title: str = Field(min_length=1, max_length=100)
    timezone: Literal["Asia/Shanghai"]
    calendar_source: str = Field(min_length=1, max_length=2000)
    night_source: str = Field(min_length=1, max_length=2000)
    calendar: list[CalendarDay] = Field(min_length=2, max_length=3700)
    periods: list[Period] = Field(min_length=1, max_length=100)
    exceptions: list[ExceptionDay] = Field(max_length=200)

    @model_validator(mode="after")
    def valid(self):
        for a, b in zip(self.calendar, self.calendar[1:]):
            if b.date - a.date != timedelta(days=1):
                raise ValueError("自然日日历必须连续、排序且无重复；不能省略休市日")
        for a, b in zip(self.periods, self.periods[1:]):
            if b.start - a.end != timedelta(days=1):
                raise ValueError("品种时段生效期间必须连续、排序且无重叠")
        if (
            self.periods[0].start <= self.calendar[0].date
            or self.periods[-1].end > self.calendar[-1].date
        ):
            raise ValueError("日历须包含规则开始前的开市日及完整生效期间")
        if not any(d.is_open and d.date < self.periods[0].start for d in self.calendar):
            raise ValueError("缺少起始交易日前的开市日依据")
        if len({e.trading_day for e in self.exceptions}) != len(self.exceptions):
            raise ValueError("特殊时段日期重复")
        for e in self.exceptions:
            if not self.periods[0].start <= e.trading_day <= self.periods[-1].end:
                raise ValueError("特殊时段超出规则范围")
            if any(s.end_offset for s in e.day):
                raise ValueError("特殊日盘不能跨自然日")
            if not next(d for d in self.calendar if d.date == e.trading_day).is_open:
                raise ValueError("休市交易日不能定义交易时段")
        spans = self.spans()
        for a, b in pairwise(spans):
            if a.end > b.start:
                raise ValueError("交易时段重叠或跨交易日冲突")
        return self

    def check_contract(self, contract: str):
        match = re.fullmatch(r"([A-Z]+)\.([A-Za-z]+)\d{3,4}", contract)
        if not match or match[1] != self.exchange or match[2].upper() != self.product:
            raise ValueError("交易时间版本与合约交易所或品种不一致")

    def spans(self) -> list["Span"]:
        return self._spans

    @cached_property
    def _spans(self) -> list["Span"]:
        values = []
        previous = None
        exceptions = {e.trading_day: e for e in self.exceptions}
        for item in self.calendar:
            if not item.is_open:
                continue
            period = next((p for p in self.periods if p.start <= item.date <= p.end), None)
            if period:
                choice = exceptions.get(item.date, period)
                for slots, anchor, phase in (
                    (choice.day, item.date, "day"),
                    (choice.night, previous.date if previous else None, "night"),
                ):
                    if phase == "night" and (previous is None or not previous.night_open):
                        continue
                    if anchor is None:
                        raise ValueError("缺少夜盘锚定自然日")
                    for slot in slots:
                        start = datetime.combine(anchor, time.fromisoformat(slot.start), ZONE)
                        end = datetime.combine(
                            anchor + timedelta(days=slot.end_offset),
                            time.fromisoformat(slot.end),
                            ZONE,
                        )
                        if phase == "night" and end >= datetime.combine(item.date, time(9), ZONE):
                            raise ValueError("夜盘不得越过目标交易日日盘边界")
                        values.append(
                            Span(
                                start=start,
                                end=end,
                                trading_day=item.date,
                                session="day" if phase == "day" else "night",
                                phase=slot.phase,
                            )
                        )
            previous = item
        return sorted(values, key=lambda s: s.start)

    def daily(self, contract: str, day: date) -> list["Span"]:
        self.check_contract(contract)
        if not self.periods[0].start <= day <= self.periods[-1].end:
            raise ValueError("交易时间版本未覆盖交易日")
        spans = [s for s in self.spans() if s.trading_day == day and s.phase == "continuous"]
        if not spans:
            raise ValueError("交易日休市或品种停盘，不能接收行情")
        return spans

    def resolve(
        self, contract: str, stamp: datetime, boundary: Literal["event", "bar_end"] = "event"
    ) -> "Span":
        self.check_contract(contract)
        if stamp.tzinfo is None:
            raise ValueError("时间戳必须包含时区")
        stamp = stamp.astimezone(ZONE)
        found = [
            s
            for s in self.spans()
            if (s.start <= stamp < s.end if boundary == "event" else s.start < stamp <= s.end)
            and (boundary != "bar_end" or s.phase == "continuous")
        ]
        if len(found) != 1:
            raise ValueError("时间不在已确认的交易时段内，或缺少时间规则覆盖")
        return found[0]

    def validate_bar(
        self,
        contract: str,
        stamp: datetime,
        day: date,
        seconds: int,
        boundary: Literal["bar_start", "bar_end"],
    ):
        span = self.resolve(contract, stamp, "bar_end" if boundary == "bar_end" else "event")
        start = stamp - timedelta(seconds=seconds) if boundary == "bar_end" else stamp
        end = start + timedelta(seconds=seconds)
        if (
            span.phase != "continuous"
            or span.trading_day != day
            or start < span.start
            or end > span.end
        ):
            raise ValueError("日内行情交易日错误或跨越休市/集合竞价边界")
        return span


class Span(Strict):
    start: AwareDatetime
    end: AwareDatetime
    trading_day: date
    session: Literal["day", "night"]
    phase: Literal["auction", "continuous"]


class TimeVersion(Strict):
    id: str = Field(pattern=r"^[a-f0-9]{64}$")
    spec: TimeSpec

    @model_validator(mode="after")
    def valid(self):
        if self.id != time_id(self.spec):
            raise ValueError("交易时间版本指纹不一致")
        return self


def time_id(spec: TimeSpec):
    return hashlib.sha256(canonical(spec.model_dump(mode="json"))).hexdigest()
