"""Cross-checks between sources. Nothing here is stored or used as data: a check
reads a second source, compares it with the first and reports every difference.

- Settlement parameters (margin rates and fees) of the exchanges' own files
  against Tushare's fut_settle. The exchanges publish them in different units;
  both sides are brought to fractions (margin, fee rate) and yuan per lot.
- Daily bars of AKShare (which reads the exchanges' sites) against the
  warehouse's Tushare bars.

DCE refuses automated reads of its site, so it has no official or AKShare side.
"""
from dataclasses import dataclass, field
from decimal import Decimal
from asterion_lab.sources.exchanges import official_settlement

FIRST_BATCH = {
    "SHFE": ["ag", "au", "cu", "al", "zn", "ni", "sn", "rb", "ru"],
    "INE": ["sc"],
    "DCE": ["i", "jm", "p", "m", "y"],
    "CZCE": ["ma", "ta", "sa"],
    "GFEX": ["lc"],
    "CFFEX": ["if"],
}
UNAVAILABLE_SETTLEMENT = {
    "DCE": "the exchange's site refuses automated reads",
    "GFEX": "no published file of settlement parameters was found",
    "CFFEX": "its parameter file has price limits and position limits, not margins or fees",
}
UNAVAILABLE_DAILY = {"DCE": "the exchange's site refuses automated reads"}
THOUSANDTH = Decimal("0.001")


@dataclass
class Group:
    exchange: str
    day: str
    unavailable: str = ""
    note: str = ""
    compared_rows: int = 0
    compared_values: int = 0
    only_first: list = field(default_factory=list)   # contracts only Tushare has
    only_second: list = field(default_factory=list)  # contracts only the second source has
    differences: list = field(default_factory=list)  # (contract, field, first, second)



def compare(group: Group, first: dict, second: dict, fields, products=None):
    keep = lambda contract: products is None or contract.split("/")[1] in products
    first = {k: v for k, v in first.items() if keep(k)}
    second = {k: v for k, v in second.items() if keep(k)}
    group.only_first = sorted(set(first) - set(second))
    group.only_second = sorted(set(second) - set(first))
    for contract in sorted(set(first) & set(second)):
        group.compared_rows += 1
        for name in fields:
            a, b = first[contract].get(name), second[contract].get(name)
            group.compared_values += 1
            if a != b and not (a is not None and b is not None and Decimal(a) == Decimal(b)):
                group.differences.append((contract, name, "" if a is None else str(a),
                                          "" if b is None else str(b)))


# What each exchange's check compares, and what it cannot.
SETTLEMENT_FIELDS = {
    "SHFE": ("long_margin_rate", "short_margin_rate", "fee_rate", "fee_per_lot"),
    "INE": ("long_margin_rate", "short_margin_rate", "fee_rate", "fee_per_lot"),
    "CZCE": ("long_margin_rate", "short_margin_rate", "fee_number", "close_today_fee_number"),
}
SETTLEMENT_NOTES = {
    "SHFE": "Tushare gives no close-today fee for SHFE; it is not compared.",
    "INE": "Tushare gives no close-today fee for INE; it is not compared.",
    "CZCE": "Tushare gives CZCE fees as the exchange's number without its basis (a ratio in "
            "ten-thousandths or yuan per lot); the numbers are compared as published.",
}


def normalize_tushare(exchange: str, rows: dict) -> dict:
    """Tushare's fut_settle in the units the official side is brought to."""
    result = {}
    for contract, values in rows.items():
        item = {"long_margin_rate": values["long_margin_rate"],
                "short_margin_rate": values["short_margin_rate"]}
        if exchange in ("SHFE", "INE"):
            # The exchange's file copied: fee ratios in thousandths of turnover.
            rate = values["trading_fee_rate"]
            item["fee_rate"] = None if rate is None else rate * THOUSANDTH
            item["fee_per_lot"] = values["trading_fee"]
        else:
            item["fee_number"] = values["trading_fee"]
            item["close_today_fee_number"] = values["offset_today_fee"]
        result[contract] = item
    return result
