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
from decimal import Decimal, InvalidOperation
import datetime
import json
import re
import ssl
import urllib.request

import certifi

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
TEN_THOUSANDTH = Decimal("0.0001")
PERCENT = Decimal("0.01")


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


def fetch(url: str, timeout: float = 30) -> bytes:
    context = ssl.create_default_context(cafile=certifi.where())
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        return response.read()


def decimal(value):
    """An exact decimal from a source's cell; None when the cell is empty."""
    if value is None or value == "":
        return None
    try:
        result = Decimal(str(value).replace(",", "").strip())
    except InvalidOperation:
        raise ValueError(f"{value!r} is not a number")
    return None if result.is_nan() else result


def month_of(digits: str, day: str) -> str:
    """YYYY-MM of a contract code's digits: four (2701) or CZCE's three (701)."""
    if len(digits) == 4:
        return f"20{digits[:2]}-{digits[2:]}"
    year = int(day[:4])
    # CZCE's one-digit year: the nearest year from last year on with that digit.
    candidate = next(y for y in range(year - 1, year + 10) if y % 10 == int(digits[0]))
    return f"{candidate}-{digits[1:]}"


def contract_of(exchange: str, code: str, day: str):
    match = re.fullmatch(r"([A-Za-z]{1,3})([0-9]{3,4})", code.strip())
    if not match:
        return None
    return f"{exchange}/{match.group(1).lower()}/{month_of(match.group(2), day)}"


def official_settlement(exchange: str, day: str) -> dict:
    """Margin rate and fees by contract, from the exchange's own file."""
    compact = day.replace("-", "")
    rows = {}
    if exchange in ("SHFE", "INE"):
        host = "www.shfe.com.cn" if exchange == "SHFE" else "www.ine.cn"
        document = json.loads(fetch(f"https://{host}/data/tradedata/future/dailydata/js{compact}.dat"))
        for row in document["o_cursor"]:
            contract = contract_of(exchange, row["INSTRUMENTID"], day)
            if contract is None:
                continue
            # Margins are fractions; fee ratios are in thousandths of turnover.
            rows[contract] = {
                "long_margin_rate": decimal(row["SPECLONGMARGINRATIO"]),
                "short_margin_rate": decimal(row["SPECSHORTMARGINRATIO"]),
                "fee_rate": decimal(row["TRADEFEERATIO"]) * THOUSANDTH,
                "fee_per_lot": decimal(row["TRADEFEEUNIT"]),
                "close_today_fee_rate": decimal(row["TTRADEFEERATIO"]) * THOUSANDTH,
                "close_today_fee_per_lot": decimal(row["TTRADEFEEUNIT"]),
            }
        return rows
    if exchange == "CZCE":
        text = fetch(f"https://www.czce.com.cn/cn/DFSStaticFiles/Future/{day[:4]}/{compact}/"
                     "FutureDataClearParams.txt").decode("utf-8")
        lines = [line for line in text.splitlines() if "|" in line]
        header = [cell.strip() for cell in lines[0].split("|")]
        for line in lines[1:]:
            cells = dict(zip(header, (cell.strip() for cell in line.split("|"))))
            contract = contract_of("CZCE", cells["合约代码"], day)
            if contract is None:
                continue
            # Margin in percent; a fee is a ratio in ten-thousandths or yuan per lot.
            ratio = cells["手续费收取方式"] == "比例值"
            if not ratio and cells["手续费收取方式"] != "绝对值":
                raise ValueError(f"CZCE {cells['合约代码']}: unknown fee basis {cells['手续费收取方式']}")
            fee, today = decimal(cells["交易手续费"]), decimal(cells["日内平今仓交易手续费"])
            margin = decimal(cells["交易保证金率(%)"]) * PERCENT
            rows[contract] = {
                "long_margin_rate": margin, "short_margin_rate": margin,
                "fee_number": fee, "close_today_fee_number": today,
                "fee_rate": fee * TEN_THOUSANDTH if ratio else Decimal(0),
                "fee_per_lot": Decimal(0) if ratio else fee,
                "close_today_fee_rate": today * TEN_THOUSANDTH if ratio else Decimal(0),
                "close_today_fee_per_lot": Decimal(0) if ratio else today,
            }
        return rows
    raise ValueError(f"no official settlement file for {exchange}")


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


def akshare_daily(exchange: str, day: str) -> dict:
    import warnings
    warnings.filterwarnings("ignore")
    import akshare
    compact = day.replace("-", "")
    frame = akshare.get_futures_daily(start_date=compact, end_date=compact, market=exchange)
    rows = {}
    for record in frame.to_dict("records"):
        contract = contract_of(exchange, str(record["symbol"]), day)
        if contract is None:
            continue

        def price(name):
            value = decimal(record.get(name))
            # An untraded contract's open, high, low and close are absent, not zero.
            return None if value == 0 else value

        rows[contract] = {"open": price("open"), "high": price("high"), "low": price("low"),
                          "close": price("close"), "settlement": decimal(record.get("settle")),
                          "previous_settlement": decimal(record.get("pre_settle")),
                          "volume": decimal(record.get("volume")),
                          "open_interest": decimal(record.get("open_interest"))}
    return rows



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
