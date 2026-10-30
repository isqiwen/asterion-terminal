"""Common data-source contract. Plugins return normalized rows, never orders."""
from dataclasses import dataclass, field
import datetime
from decimal import Decimal, InvalidOperation
import re

EXCHANGES = ("SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX")


class SourceError(Exception):
    """A public, credential-free source failure."""
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class Query:
    table: str
    exchange: str
    day: str
    contract: str = ""

    def validate(self):
        datetime.date.fromisoformat(self.day)
        if self.exchange not in EXCHANGES:
            raise ValueError("unknown exchange")
        if self.contract and not re.fullmatch(
                re.escape(self.exchange) + r"/[a-z]{1,3}/[0-9]{4}-(0[1-9]|1[0-2])", self.contract):
            raise ValueError("contract must be EXCHANGE/product/YYYY-MM")


@dataclass
class Batch:
    rows: list
    # Recorded observation intervals are evidence of reception, not a calendar.
    observations: list = field(default_factory=list)


class Source:
    identity = ""
    tables = ()
    exchanges = EXCHANGES
    requires_credential = False
    requires_endpoint = False

    def __init__(self, cancelled=lambda: False):
        self.cancelled = cancelled
        self.requests = 0

    def check(self, query):
        query.validate()
        if query.table not in self.tables or query.exchange not in self.exchanges:
            raise SourceError("unsupported", f"{self.identity} does not provide {query.table} for {query.exchange}")
        if self.cancelled():
            raise SourceError("cancelled", "acquisition cancelled")

    def coverage(self):
        # An API being callable does not establish its historical coverage.
        return {"known": False, "days": [], "note": "coverage is determined by each returned partition"}

    def read(self, query: Query) -> Batch:
        raise NotImplementedError


def decimal(value):
    if value is None or value == "":
        return None
    try:
        result = Decimal(str(value).replace(",", "").strip())
    except InvalidOperation:
        raise ValueError("source returned a non-numeric value")
    if result.is_nan():
        return None
    if not result.is_finite():
        raise ValueError("source returned a non-finite number")
    return result


def month_of(digits, day):
    if len(digits) == 4:
        month = f"20{digits[:2]}-{digits[2:]}"
    else:
        year = int(day[:4])
        candidate = next(y for y in range(year - 1, year + 10) if y % 10 == int(digits[0]))
        month = f"{candidate}-{digits[1:]}"
    datetime.date.fromisoformat(month + "-01")
    return month


def contract_of(exchange, code, day):
    match = re.fullmatch(r"([A-Za-z]{1,3})([0-9]{3,4})", code.strip())
    return None if not match else f"{exchange}/{match.group(1).lower()}/{month_of(match.group(2), day)}"
