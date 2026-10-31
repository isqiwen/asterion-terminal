"""Tushare futures tables: contracts (fut_basic), calendars (trade_cal), daily
bars (fut_daily) and a contract's one-minute bars (ft_mins).

The mapping rules are those the C++ Data service learned from real answers.
Numbers are
read from the JSON text as exact decimals. The token is sent to Tushare and
nowhere else: no error, log line or stored row repeats it.
"""
from dataclasses import dataclass, field
import datetime
from decimal import Decimal
import json
import re
import ssl
import threading
import time
import urllib.error
import urllib.request

import certifi

URL = "https://api.tushare.pro"
from asterion_lab.sources.base import EXCHANGES, SourceError
BEIJING = datetime.timezone(datetime.timedelta(hours=8))
# ft_mins answers at most a page of rows and takes ranges under 31 days.
MINUTE_PAGE = 8000
MINUTE_DAYS = 27
SUFFIXES = {"SHFE": ".SHF", "DCE": ".DCE", "CZCE": ".ZCE", "CFFEX": ".CFX", "INE": ".INE",
            "GFEX": ".GFE"}



def classify(message: str, code: str) -> str:
    if "token" in message and ("不正确" in message or "无效" in message):
        return "invalid_credential"
    if any(word in message for word in ("每分钟", "频率", "每小时", "每天最多")):
        return "rate_limit"
    # Tushare documents 2002 as an access-permission error.
    return "permission" if code == "2002" else "failed"


REFUSALS = {"invalid_credential": "Tushare did not accept the token",
            "rate_limit": "the Tushare account reached its request limit",
            "permission": "the Tushare account has no permission for",
            "failed": "Tushare reported an error for"}


class Budget:
    """At most `per_minute` requests in any sixty seconds, shared by every reader."""

    def __init__(self, per_minute: int):
        if not 1 <= per_minute <= 500:
            raise ValueError("requests per minute must be 1 to 500")
        self.per_minute = per_minute
        self.sent = []
        self.lock = threading.Lock()

    def acquire(self, cancelled):
        while True:
            with self.lock:
                now = time.monotonic()
                self.sent = [moment for moment in self.sent if now - moment < 60]
                if len(self.sent) < self.per_minute:
                    self.sent.append(now)
                    return
                wait = 60 - (now - self.sent[0])
            if cancelled():
                raise SourceError("cancelled", "acquisition cancelled")
            time.sleep(min(wait, 0.5))


def https_post(body: bytes, timeout: float = 60) -> bytes:
    context = ssl.create_default_context(cafile=certifi.where())
    request = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        return response.read()


@dataclass
class Table:
    fields: dict
    rows: list

    def at(self, row, name):
        return row[self.fields[name]]


@dataclass
class Listing:
    code: str
    contract: str
    product: str
    list_date: str
    delist_date: str
    row: dict = field(default_factory=dict)


def dashed(value) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{8}", value):
        raise ValueError("is not a YYYYMMDD date")
    return f"{value[:4]}-{value[4:6]}-{value[6:]}"


def optional_date(value):
    return None if value in (None, "") else dashed(value)


def label(value):
    if value is None:
        return None
    text = "".join(" " if ord(c) < 0x20 or c == "\x7f" else c for c in str(value)).strip()
    return text or None


def number(value, shift: int = 0):
    """An exact decimal, or None. `shift` moves the point (amounts are in 10,000s)."""
    if value is None:
        return None
    if not isinstance(value, (Decimal, int)):
        raise ValueError("is not a number")
    return Decimal(value).scaleb(shift)


def dated(code: str, suffix: str) -> bool:
    """A contract the identity covers: one to three letters and a delivery month.
    Other codes are the vendor's own main and continuous series (RB.SHF,
    RBL.SHF) and contracts the identity cannot express (L2605F.DCE)."""
    return bool(code.endswith(suffix)
                and re.fullmatch(r"[A-Z]{1,3}[0-9]{3,4}", code[:-len(suffix)]))


def czce_code(code: str) -> str:
    """What CZCE itself calls a contract: letters and three digits, no decade."""
    base = code[:-len(".ZCE")]
    letters = re.match(r"[A-Z]+", base).group(0)
    return letters + base[-3:]


class Tushare:
    def __init__(self, token: str, budget: Budget, post=https_post, cancelled=lambda: False):
        if not token or len(token) > 256 or any(c in token for c in "\r\n\t "):
            raise SourceError("invalid_request", "Tushare token is required without whitespace")
        self.token = token
        self.budget = budget
        self.post = post
        self.cancelled = cancelled
        self.requests = 0
        self.catalogues = {}
        self.calendars = {}  # (exchange, year) -> day -> calendar row
        self.windows = {}  # contract -> the minutes last read for it

    def private(self, text: str) -> bool:
        return self.token in text

    def request(self, interface: str, params: dict, fields: str, partition: str) -> Table:
        if self.cancelled():
            raise SourceError("cancelled", "acquisition cancelled")
        self.budget.acquire(self.cancelled)
        self.requests += 1
        body = json.dumps({"api_name": interface, "token": self.token, "params": params,
                           "fields": fields}).encode()
        try:
            raw = self.post(body)
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            reason = getattr(error, "reason", error)
            raise SourceError("unavailable", f"{partition}: Tushare could not be reached ({type(reason).__name__})")
        try:
            # Exact decimals: never through binary floating point.
            root = json.loads(raw, parse_float=Decimal)
            code = str(root["code"])
        except (ValueError, KeyError, TypeError):
            raise SourceError("failed", f"{partition}: the answer of {interface} is not JSON")
        if code != "0":
            message = str(root.get("msg") or "")
            reason = classify(message, code)
            raise SourceError(reason, f"{REFUSALS[reason]} {interface}")
        try:
            names = root["data"]["fields"]
            rows = root["data"]["items"]
            fields_at = {name: index for index, name in enumerate(names)}
            if len(fields_at) != len(names) or any(len(row) != len(names) for row in rows):
                raise ValueError
            for name in fields.split(","):
                if name not in fields_at:
                    raise ValueError
        except (KeyError, TypeError, ValueError):
            raise SourceError("failed", f"{partition}: the answer of {interface} is not the expected table")
        return Table(fields_at, rows)

    def ask(self, interface, params, fields, limit, partition) -> Table:
        table = self.request(interface, params, fields, partition)
        # The vendor stops at its limit without saying so.
        if len(table.rows) >= limit:
            raise SourceError("failed", f"{partition}: {interface} returned its limit of {limit} rows, "
                                        "so the partition may be incomplete")
        return table

    def contracts(self, exchange: str) -> list:
        if exchange in self.catalogues:
            return self.catalogues[exchange]
        partition = f"futures.contracts {exchange}"
        table = self.ask("fut_basic", {"exchange": exchange, "fut_type": "1"},
                         "ts_code,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,"
                         "quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate,"
                         "trade_time_desc", 10000, partition)
        listings = []
        for row in table.rows:
            code = row[table.fields["ts_code"]] or ""
            try:
                if table.at(row, "exchange") != exchange:
                    raise ValueError("belongs to another exchange")
                if not dated(code, SUFFIXES[exchange]):
                    continue
                month = table.at(row, "d_month")
                if not isinstance(month, str) or len(month) != 6:
                    raise ValueError("has no delivery month")
                product = str(table.at(row, "fut_code")).lower()
                delivery = f"{month[:4]}-{month[4:]}"
                digits = re.search(r"[0-9]+", code).group(0)
                expected = delivery[3] + delivery[5:] if len(digits) == 3 else delivery[2:4] + delivery[5:]
                if re.match(r"[A-Z]+", code).group(0).lower() != product or digits != expected:
                    raise ValueError("does not match its product and delivery month")
                listing = Listing(code, f"{exchange}/{product}/{delivery}", product,
                                  dashed(table.at(row, "list_date")),
                                  dashed(table.at(row, "delist_date")))
                listing.row = {
                    "exchange": exchange, "contract": listing.contract, "product": product,
                    "delivery_month": delivery, "source_instrument": code,
                    "name": label(table.at(row, "name")),
                    "list_date": listing.list_date, "delist_date": listing.delist_date,
                    "last_delivery_date": optional_date(table.at(row, "last_ddate")),
                    "multiplier": number(table.at(row, "multiplier")),
                    "per_unit": number(table.at(row, "per_unit")),
                    "trade_unit": label(table.at(row, "trade_unit")),
                    "quote_unit": label(table.at(row, "quote_unit")),
                    "tick_description": label(table.at(row, "quote_unit_desc")),
                    "delivery_description": label(table.at(row, "d_mode_desc")),
                    "trading_hours": label(table.at(row, "trade_time_desc")),
                }
                listings.append(listing)
            except (ValueError, TypeError, AttributeError) as error:
                raise SourceError("failed", f"{partition}: contract {code or 'without a code'}: {error}")
        self.catalogues[exchange] = listings
        return listings

    def listed(self, exchange: str, code: str, day: str) -> Listing:
        listings = self.contracts(exchange)
        live = lambda listing: listing.list_date <= day <= listing.delist_date
        exact = next((listing for listing in listings if listing.code == code), None)
        if exchange == "CZCE" and (exact is None or not live(exact)):
            # CZCE writes the year as one digit, so its codes repeat every ten years,
            # and the decade the vendor adds is not always right (AP1003 for the
            # AP003 listed in 2019). Letters and the last three digits then name the
            # contract listed that day.
            own = czce_code(code)
            meant = next((listing for listing in listings
                          if live(listing) and czce_code(listing.code) == own), None)
            if meant:
                return meant
        if exact is None:
            raise ValueError("is not in the contract list")
        return exact

    def calendar(self, exchange: str, year: int) -> list:
        partition = f"futures.calendar {exchange} {year}"
        table = self.ask("trade_cal", {"exchange": exchange, "start_date": f"{year}0101",
                                       "end_date": f"{year}1231"},
                         "exchange,cal_date,is_open,pretrade_date", 2000, partition)
        rows = []
        for row in table.rows:
            try:
                if table.at(row, "exchange") != exchange:
                    raise ValueError("belongs to another exchange")
                rows.append({"exchange": exchange, "day": dashed(table.at(row, "cal_date")),
                             "open": str(table.at(row, "is_open")) == "1",
                             "previous_trading_day": optional_date(table.at(row, "pretrade_date"))})
            except (ValueError, TypeError) as error:
                raise SourceError("failed", f"{partition}: a calendar day {error}")
        return rows

    def daily(self, exchange: str, day: str) -> list:
        partition = f"futures.daily {exchange} {day}"
        self.contracts(exchange)
        compact = day.replace("-", "")
        table = self.ask("fut_daily", {"trade_date": compact, "exchange": exchange},
                         "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,"
                         "amount,oi,oi_chg,delv_settle", 2000, partition)
        rows = []
        for row in table.rows:
            code = row[table.fields["ts_code"]] or ""
            try:
                if not code.endswith(SUFFIXES[exchange]) or table.at(row, "trade_date") != compact:
                    raise ValueError("belongs to another exchange or day")
                if not dated(code, SUFFIXES[exchange]):
                    continue
                contract = self.listed(exchange, code, day)

                # A contract nobody traded in continuous matching has no open, high
                # or low that day. SHFE and INE send nulls; CZCE, DCE and GFEX send
                # zeros. No price of these markets is zero: it is the absence of one.
                def traded(name):
                    value = number(table.at(row, name))
                    return None if value == 0 else value

                prices = {name: traded(name) for name in ("open", "high", "low", "close")}
                # Without a trade there is no close either: for CZCE the vendor puts
                # the settlement price there while the exchange publishes none (the
                # cross-check with the exchange's own file found it).
                if prices["open"] is None and prices["high"] is None and prices["low"] is None:
                    prices["close"] = None
                rows.append({
                    "exchange": exchange, "product": contract.product,
                    "contract": contract.contract, "day": day, **prices,
                    "settlement": number(table.at(row, "settle")),
                    "previous_close": number(table.at(row, "pre_close")),
                    "previous_settlement": number(table.at(row, "pre_settle")),
                    "volume": number(table.at(row, "vol")),
                    # Published in units of ten thousand.
                    "amount": number(table.at(row, "amount"), 4),
                    "open_interest": number(table.at(row, "oi")),
                    "open_interest_change": number(table.at(row, "oi_chg")),
                    "delivery_settlement": number(table.at(row, "delv_settle")),
                })
            except (ValueError, TypeError, AttributeError) as error:
                if self.private(code) or self.private(str(error)):
                    raise SourceError("failed", f"{partition}: the answer contains private text")
                raise SourceError("failed", f"{partition}: {code or 'a row without a code'} {error}")
        return rows

    def settlement(self, exchange: str, day: str) -> dict:
        """Settlement parameters by contract, as Tushare's fut_settle gives them.
        Only for cross-checks: they are not stored."""
        partition = f"futures.settlement {exchange} {day}"
        self.contracts(exchange)
        compact = day.replace("-", "")
        table = self.ask("fut_settle", {"trade_date": compact, "exchange": exchange},
                         "ts_code,trade_date,trading_fee_rate,trading_fee,delivery_fee,"
                         "b_hedging_margin_rate,s_hedging_margin_rate,long_margin_rate,"
                         "short_margin_rate,offset_today_fee", 1600, partition)
        rows = {}
        for row in table.rows:
            code = row[table.fields["ts_code"]] or ""
            try:
                if not code.endswith(SUFFIXES[exchange]) or table.at(row, "trade_date") != compact:
                    raise ValueError("belongs to another exchange or day")
                if not dated(code, SUFFIXES[exchange]):
                    continue
                contract = self.listed(exchange, code, day)
                # CFFEX goes on repeating a contract's parameters after its last day.
                if not contract.list_date <= day <= contract.delist_date:
                    continue
                rows[contract.contract] = {
                    name: number(table.at(row, name)) for name in (
                        "trading_fee_rate", "trading_fee", "offset_today_fee",
                        "long_margin_rate", "short_margin_rate")}
            except (ValueError, TypeError, AttributeError) as error:
                raise SourceError("failed", f"{partition}: {code or 'a row without a code'} {error}")
        return rows

    def calendar_day(self, exchange: str, day: str) -> dict | None:
        """The calendar row of a day, the year's calendar read once."""
        key = (exchange, int(day[:4]))
        if key not in self.calendars:
            self.calendars[key] = {row["day"]: row for row in self.calendar(*key)}
        return self.calendars[key].get(day)

    def trading_day(self, exchange: str, end: datetime.datetime) -> str:
        """The trading day of a minute that ended at `end`, Beijing time: the
        day session's own day, or for the night session the next open day."""
        def is_open(day):
            row = self.calendar_day(exchange, day.isoformat())
            if row is None:
                raise ValueError(f"the calendar does not cover {day}")
            return row["open"]

        if 6 <= end.hour < 18:
            if not is_open(end.date()):
                raise ValueError(f"a minute at {end} falls on a closed day")
            return end.date().isoformat()
        evening = end.date() if end.hour >= 18 else end.date() - datetime.timedelta(days=1)
        if not is_open(evening):
            raise ValueError(f"a night minute at {end} follows a closed day")
        for n in range(1, 32):
            if is_open(evening + datetime.timedelta(days=n)):
                return (evening + datetime.timedelta(days=n)).isoformat()
        raise ValueError(f"no open day follows the night of {evening}")

    def minutes(self, contract: str, day: str) -> list:
        """A contract's one-minute bars of a trading day. One request window
        covers up to MINUTE_DAYS days from the day asked, so reading the days
        of a contract in order asks Tushare about once a month."""
        window = self.windows.get(contract)
        if window and day in window["days"]:
            return window["rows"].get(day, [])
        exchange, product, _ = contract.split("/")
        row = self.calendar_day(exchange, day)
        listing = next((item for item in self.contracts(exchange) if item.contract == contract), None)
        if listing is None:
            raise SourceError("failed", f"Tushare lists no contract {contract}")
        if row is None or not row["open"] or not listing.list_date <= day <= listing.delist_date:
            return []
        first = datetime.date.fromisoformat(day)
        last = min(first + datetime.timedelta(days=MINUTE_DAYS),
                   datetime.date.fromisoformat(listing.delist_date))
        # From the evening the trading day's night session opens.
        begin = row["previous_trading_day"] or (first - datetime.timedelta(days=1)).isoformat()
        partition = f"futures.minutes {contract} {day}"
        params = {"ts_code": listing.code, "freq": "1min", "start_date": f"{begin} 18:00:00",
                  "end_date": f"{last.isoformat()} 15:30:00", "limit": MINUTE_PAGE}
        fields = "ts_code,trade_time,open,high,low,close,vol,amount,oi"
        bars, seen = {}, set()
        while True:
            table = self.request("ft_mins", {**params, "offset": len(seen)}, fields, partition)
            for item in table.rows:
                try:
                    if table.at(item, "ts_code") != listing.code:
                        raise ValueError("belongs to another contract")
                    end = datetime.datetime.fromisoformat(str(table.at(item, "trade_time")))
                    if end.second or end in seen:
                        raise ValueError(f"has a repeated or unrounded time {end}")
                    # Commodity exchanges pause 10:15 to 10:30: a minute labelled 10:30
                    # would have begun in the pause, so the labels are not minute ends.
                    if exchange != "CFFEX" and (end.hour, end.minute) == (10, 30):
                        raise ValueError("labels minutes by their start, not their end")
                    volume = number(table.at(item, "vol"))
                    if volume is None or volume != volume.to_integral_value():
                        raise ValueError(f"has a volume of {volume} at {end}")
                    seen.add(end)
                    traded = self.trading_day(exchange, end)
                    start = end.replace(tzinfo=BEIJING) - datetime.timedelta(minutes=1)
                    bars.setdefault(traded, []).append({
                        "exchange": exchange, "product": product, "contract": contract,
                        "day": traded, "start_ms": int(start.timestamp()) * 1000, "partial": False,
                        **{name: number(table.at(item, name)) for name in
                           ("open", "high", "low", "close")},
                        "volume": volume, "average_price": None,
                        "open_interest": number(table.at(item, "oi"))})
                except (ValueError, TypeError) as error:
                    raise SourceError("failed", f"{partition}: {error}")
            if len(table.rows) < MINUTE_PAGE:
                break
        # The open days the window holds whole; a later night belongs to the next.
        days = set()
        for n in range((last - first).days + 1):
            current = (first + datetime.timedelta(days=n)).isoformat()
            entry = self.calendar_day(exchange, current)
            if entry and entry["open"]:
                days.add(current)
        rows = {key: sorted(value, key=lambda bar: bar["start_ms"])
                for key, value in bars.items() if key in days}
        self.windows[contract] = {"days": days, "rows": rows}
        return rows.get(day, [])
