"""Trading sessions with effective dates, from reviewed exchange documents.

The rules are in reference/trading_sessions.json: each names its sources and
says whether a source announces its start ("start") or only shows it in force
from a date ("confirmed"). Outside every rule a session is unknown, never
assumed. A night session belongs to the next trading day and is held on the
evening of the trading day before it, except before a statutory holiday.
"""
from dataclasses import dataclass, field
import datetime
import json
from pathlib import Path
import re

REFERENCE = Path(__file__).parent / "reference" / "trading_sessions.json"
TIME = re.compile(r"([01][0-9]|2[0-3]):[0-5][0-9]")


@dataclass
class Night:
    state: str  # held, none, holiday_eve, unknown
    begin: str = ""  # "YYYY-MM-DD HH:MM"
    end: str = ""
    sources: list = field(default_factory=list)


@dataclass
class Day:
    known: bool
    segments: list = field(default_factory=list)  # [("YYYY-MM-DD HH:MM", ...)]
    sources: list = field(default_factory=list)


def day_of(text: str) -> datetime.date:
    return datetime.date.fromisoformat(text)


class Sessions:
    def __init__(self, path: Path = REFERENCE):
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("format") != 1:
            raise ValueError(f"unsupported trading session reference: {path}")
        self.reviewed = document["reviewed"]
        self.sources = document["sources"]
        self.days = document["day_sessions"]
        self.nights = document["night_sessions"]
        self.unknown = document["unknown_evenings"]
        self.holiday_sources = document["holiday_eve"]["sources"]
        self.validate(path)

    def validate(self, path: Path):
        def check(rule, what):
            for source in rule["sources"]:
                if source not in self.sources:
                    raise ValueError(f"{path}: {what} names an unknown source {source}")
            if rule.get("basis", "start") not in ("start", "confirmed"):
                raise ValueError(f"{path}: {what} has an unknown basis")
            day_of(rule["from"])
            if "to" in rule and day_of(rule["to"]) < day_of(rule["from"]):
                raise ValueError(f"{path}: {what} ends before it begins")

        for rule in self.days:
            check(rule, f"day session of {rule['exchanges']}")
            times = [time for segment in rule["segments"] for time in segment]
            if not all(TIME.fullmatch(time) for time in times) or times != sorted(times):
                raise ValueError(f"{path}: day session of {rule['exchanges']} is not in order")
        claimed = {}
        for rule in self.nights:
            check(rule, f"night session of {rule['exchange']} {rule['products']}")
            if rule["end"] is not None and not TIME.fullmatch(rule["end"]):
                raise ValueError(f"{path}: night end of {rule['products']} is not a time")
            for product in rule["products"]:
                for other in claimed.setdefault((rule["exchange"], product), []):
                    if day_of(rule["from"]) <= day_of(other.get("to", "9999-12-31")) and \
                            day_of(other["from"]) <= day_of(rule.get("to", "9999-12-31")):
                        raise ValueError(f"{path}: night sessions of {product} overlap")
                claimed[(rule["exchange"], product)].append(rule)
        for rule in self.unknown:
            check(rule, f"unknown evenings of {rule['exchanges']}")
        for source in self.holiday_sources:
            if source not in self.sources:
                raise ValueError(f"{path}: the holiday rule names an unknown source {source}")

    @staticmethod
    def covers(rule, day: datetime.date) -> bool:
        return day_of(rule["from"]) <= day <= day_of(rule.get("to", "9999-12-31"))

    def day(self, exchange: str, product: str, trading_day: datetime.date) -> Day:
        for rule in self.days:
            if exchange in rule["exchanges"] and product in rule.get("products", [product]) \
                    and self.covers(rule, trading_day):
                stamp = trading_day.isoformat()
                return Day(True, [(f"{stamp} {a}", f"{stamp} {b}") for a, b in rule["segments"]],
                           rule["sources"])
        return Day(False)

    def night(self, exchange: str, product: str, evening: datetime.date,
              holiday_follows: bool) -> Night:
        """The night session held on `evening`, the trading day before the one it belongs to."""
        for rule in self.unknown:
            if exchange in rule["exchanges"] and self.covers(rule, evening):
                return Night("unknown", sources=rule["sources"])
        for rule in self.nights:
            if rule["exchange"] == exchange and product in rule["products"] and \
                    self.covers(rule, evening):
                if rule["end"] is None:
                    return Night("none", sources=rule["sources"])
                if holiday_follows:
                    return Night("holiday_eve", sources=self.holiday_sources)
                end_day = evening if rule["end"] > "21:00" else evening + datetime.timedelta(days=1)
                return Night("held", f"{evening.isoformat()} 21:00",
                             f"{end_day.isoformat()} {rule['end']}", rule["sources"])
        return Night("unknown")


def holiday_between(previous: datetime.date, trading_day: datetime.date) -> bool:
    """Whether a weekday closed for a holiday lies between two trading days."""
    day = previous + datetime.timedelta(days=1)
    while day < trading_day:
        if day.weekday() < 5:
            return True
        day += datetime.timedelta(days=1)
    return False
