"""Trading-calendar value conversion and indexed immutable handle operations."""

from datetime import date, datetime
from typing import TYPE_CHECKING, ClassVar, Literal

from ._models import NativeModel

if TYPE_CHECKING:
    from .calendar import Span, TimeSpec

__all__ = ["CalendarModel", "CalendarOps", "time_id"]


class CalendarModel(NativeModel):
    _domain: ClassVar[str] = "calendar"


class CalendarOps(NativeModel):
    def check_contract(self, contract: str):
        self._call("check_contract", {"contract": contract})

    def spans(self) -> list["Span"]:
        from .calendar import Span

        return [Span.model_validate(item) for item in self._call("spans", {})]

    def daily(self, contract: str, day: date) -> list["Span"]:
        from .calendar import Span

        return [
            Span.model_validate(item)
            for item in self._call("daily", {"contract": contract, "day": day.isoformat()})
        ]

    def resolve(
        self, contract: str, stamp: datetime, boundary: Literal["event", "bar_end"] = "event"
    ) -> "Span":
        from .calendar import Span

        return Span.model_validate(
            self._call(
                "resolve", {"contract": contract, "stamp": stamp.isoformat(), "boundary": boundary}
            )
        )

    def validate_bar(
        self,
        contract: str,
        stamp: datetime,
        day: date,
        seconds: int,
        boundary: Literal["bar_start", "bar_end"],
    ) -> "Span":
        from .calendar import Span

        return Span.model_validate(
            self._call(
                "validate_bar",
                {
                    "contract": contract,
                    "stamp": stamp.isoformat(),
                    "day": day.isoformat(),
                    "seconds": seconds,
                    "boundary": boundary,
                },
            )
        )


def time_id(spec: "TimeSpec") -> str:
    return spec._call("id", {})
