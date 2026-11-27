"""Pinned research dataset loading and contract/dominant-series input preparation.

Shared by backtests, factor evaluation, rolling validation and live reconciliation:

- A contract has a daily bar when it traded in the auction (open,
  high and low present) and settled; the bar is labelled 15:00 Shanghai.
- Its multiplier is the contract table's, times the lot in the unit the
  price is quoted per; most days with turnover must agree with it within 5%.
- A contract's price increment comes from the pinned price-tick history;
  its effective intervals must cover each replayed trading day.
- A product's dominant series (symbol EXCHANGE/product) trades its dominant
  months by the rule of dominant.py; a later month joins a day before its
  roll. Months remain available through the requested range or delisting so
  paused positions can settle and roll after a new target.
"""
from bisect import bisect_left
import datetime
from decimal import Decimal, ROUND_HALF_UP
import functools
import math
from pathlib import Path

import polars as pl

from asterion_lab import chart, dominant, pit
from asterion_lab.warehouse_client import Dataset

R = 10**8
BEIJING = datetime.timezone(datetime.timedelta(hours=8))
_DAILY_RAW_COLS = (
    "open",
    "high",
    "low",
    "close",
    "settlement",
    "volume",
    "open_interest",
    "lower_limit",
    "upper_limit",
)
_SETTLEMENT_RAW_COLS = (
    "long_margin_rate",
    "short_margin_rate",
    "fee_rate",
    "fee_per_lot",
    "close_today_fee_rate",
    "close_today_fee_per_lot",
)
COSTS = (
    "margin_per_lot",
    "open_fee",
    "close_today_fee",
    "close_yesterday_fee",
    "margin_rate",
    "open_fee_rate",
    "close_today_fee_rate",
    "close_yesterday_fee_rate",
)


def raw(value) -> int:
    """An exact decimal of at most eight places as the engine's integer."""
    value = Decimal(value)
    units = value * R
    if units != units.to_integral_value():
        raise ValueError(f"{value} has more than eight decimal places")
    return int(units)


def _dec(raw_value: int | None) -> Decimal | None:
    return None if raw_value is None else Decimal(raw_value).scaleb(-8)


def scale_fee(value, multiplier: Decimal) -> int:
    """Scale an exchange commission fee by `multiplier`, rounded to engine precision."""
    scaled = (Decimal(value) * multiplier).quantize(Decimal("0.00000001"), rounding=ROUND_HALF_UP)
    return raw(scaled)


def _scale_fee_raw(value_raw: int, multiplier: Decimal) -> int:
    if multiplier == 1:
        return value_raw
    return scale_fee(Decimal(value_raw).scaleb(-8), multiplier)


def _limit_units_raw(units: int, tick: int) -> int:
    above = units + R // 1000
    return above if above % tick == 0 and units % tick else units


def limit_units(value, tick: int) -> int:
    """Tushare stores 69 SHFE gold limits exactly 0.001 below their price tick."""
    return _limit_units_raw(raw(value), tick)


@functools.lru_cache(maxsize=8192)
def close_ns(day: str) -> int:
    # 15:00 Asia/Shanghai is 07:00 UTC.
    return int(datetime.datetime.fromisoformat(f"{day}T07:00:00+00:00").timestamp()) * 10**9


class Daily:
    """What one contract did on each day read: auction prices, settlement,
    volume, turnover and open interest."""

    def __init__(self, contract: str, days: dict | None = None, *, frame: pl.DataFrame | None = None):
        self.contract = contract
        self._days = days if days is not None else (None if frame is not None else {})
        self._frame = frame
        self._cols = None
        self._index: dict[str, int] | None = None

    def _ensure_cols(self):
        if self._cols is None:
            f = self._frame
            day_col = f["day"].to_list()
            self._cols = (
                day_col,
                f["open"].to_list(),
                f["high"].to_list(),
                f["low"].to_list(),
                f["close"].to_list(),
                f["settlement"].to_list(),
                f["volume"].to_list(),
                f["open_interest"].to_list(),
                f["lower_limit"].to_list(),
                f["upper_limit"].to_list(),
                f["amount"].to_list(),
            )
            self._index = {d: i for i, d in enumerate(day_col)}
        return self._cols

    @property
    def days(self) -> dict:
        if self._days is None:
            (
                day_col,
                open_c,
                high_c,
                low_c,
                close_c,
                settle_c,
                vol_c,
                oi_c,
                lower_c,
                upper_c,
                amt_c,
            ) = self._ensure_cols()
            materialized = {}
            for i, d in enumerate(day_col):
                o, h, lo = open_c[i], high_c[i], low_c[i]
                materialized[d] = dict(
                    auction=None not in (o, h, lo),
                    open=_dec(o),
                    high=_dec(h),
                    low=_dec(lo),
                    close=_dec(close_c[i]),
                    settlement=_dec(settle_c[i]),
                    volume=_dec(vol_c[i]),
                    amount=amt_c[i],
                    open_interest=_dec(oi_c[i]),
                    lower_limit=_dec(lower_c[i]),
                    upper_limit=_dec(upper_c[i]),
                )
            self._days = materialized
        return self._days

    def has_day_in(self, begin: str, end: str) -> bool:
        if self._days is not None:
            return any(begin <= day <= end for day in self._days)
        day_col = self._ensure_cols()[0]
        idx = bisect_left(day_col, begin)
        return idx < len(day_col) and day_col[idx] <= end

    def bars(self, begin: str, end: str) -> list:
        if self._days is not None:
            return [
                day
                for day, row in sorted(self._days.items())
                if begin <= day <= end and row["auction"] and row["settlement"] is not None
            ]
        day_col, open_c, high_c, low_c, _, settle_c, *_ = self._ensure_cols()
        lo = bisect_left(day_col, begin)
        out = []
        for i in range(lo, len(day_col)):
            d = day_col[i]
            if d > end:
                break
            if open_c[i] is not None and high_c[i] is not None and low_c[i] is not None and settle_c[i] is not None:
                out.append(d)
        return out

    def row_raw(self, day: str):
        """Return `(auction, open, high, low, close, settlement, volume, oi_float, lower, upper)` in raw units."""
        if self._days is not None:
            row = self._days.get(day)
            if row is None:
                return None
            return (
                row["auction"],
                None if row["open"] is None else raw(row["open"]),
                None if row["high"] is None else raw(row["high"]),
                None if row["low"] is None else raw(row["low"]),
                None if row["close"] is None else raw(row["close"]),
                None if row["settlement"] is None else raw(row["settlement"]),
                None if row["volume"] is None else raw(row["volume"]),
                math.nan if row["open_interest"] is None else float(row["open_interest"]),
                None if row["lower_limit"] is None else raw(row["lower_limit"]),
                None if row["upper_limit"] is None else raw(row["upper_limit"]),
            )
        cols = self._ensure_cols()
        i = self._index.get(day)
        if i is None:
            return None
        o, h, lo = cols[1][i], cols[2][i], cols[3][i]
        oi = cols[7][i]
        return (
            o is not None and h is not None and lo is not None,
            o,
            h,
            lo,
            cols[4][i],
            cols[5][i],
            cols[6][i],
            math.nan if oi is None else oi / R,
            cols[8][i],
            cols[9][i],
        )

    def settlements_raw(self, begin: str, end: str) -> list[tuple[str, int]]:
        if self._days is not None:
            return [
                (day, raw(row["settlement"]))
                for day, row in sorted(self._days.items())
                if begin <= day <= end and row["settlement"] is not None
            ]
        day_col, _, _, _, _, settle_c, *_ = self._ensure_cols()
        lo = bisect_left(day_col, begin)
        out = []
        for i in range(lo, len(day_col)):
            d = day_col[i]
            if d > end:
                break
            s = settle_c[i]
            if s is not None:
                out.append((d, s))
        return out

    def anchor_before(self, begin: str) -> str | None:
        if self._days is not None:
            return max(
                (day for day, row in self._days.items() if day < begin and row["settlement"] is not None),
                default=None,
            )
        day_col, _, _, _, _, settle_c, *_ = self._ensure_cols()
        idx = bisect_left(day_col, begin) - 1
        while idx >= 0:
            if settle_c[idx] is not None:
                return day_col[idx]
            idx -= 1
        return None

    def settlement_and_oi_raw(self, anchor: str, end: str) -> tuple[dict[str, int], dict[str, int]]:
        if self._days is not None:
            settlement, oi = {}, {}
            for day, row in self._days.items():
                if anchor <= day <= end:
                    if row["settlement"] is not None:
                        settlement[day] = raw(row["settlement"])
                    if row["open_interest"] is not None:
                        oi[day] = raw(row["open_interest"])
            return settlement, oi
        day_col, _, _, _, _, settle_c, _, oi_c, *_ = self._ensure_cols()
        lo = bisect_left(day_col, anchor)
        settlement, oi = {}, {}
        for i in range(lo, len(day_col)):
            d = day_col[i]
            if d > end:
                break
            s = settle_c[i]
            if s is not None:
                settlement[d] = s
            o = oi_c[i]
            if o is not None:
                oi[d] = o
        return settlement, oi

    def prev_auction_closes_raw(self) -> dict[str, int]:
        if self._days is not None:
            prev_map, last_close = {}, None
            for d, row in sorted(self._days.items()):
                if row["auction"] and row["close"] is not None:
                    c_val = raw(row["close"])
                    if last_close is not None and last_close > 0:
                        prev_map[d] = last_close
                    last_close = c_val
            return prev_map
        day_col, open_c, high_c, low_c, close_c, *_ = self._ensure_cols()
        prev_map, last_close = {}, None
        for i, d in enumerate(day_col):
            if open_c[i] is not None and high_c[i] is not None and low_c[i] is not None:
                c_val = close_c[i]
                if c_val is not None:
                    if last_close is not None and last_close > 0:
                        prev_map[d] = last_close
                    last_close = c_val
        return prev_map

    def auction_close_raw(self, day: str) -> int | None:
        if self._days is not None:
            row = self._days.get(day)
            if row and row["auction"] and row["close"] is not None:
                return raw(row["close"])
            return None
        cols = self._ensure_cols()
        i = self._index.get(day)
        if i is None:
            return None
        if cols[1][i] is not None and cols[2][i] is not None and cols[3][i] is not None:
            return cols[4][i]
        return None

    def implied_multipliers(self, begin: str, end: str) -> list[Decimal]:
        if self._days is not None:
            return [
                Decimal(row["amount"]) / Decimal(row["volume"]) / Decimal(row["settlement"])
                for day, row in self._days.items()
                if begin <= day <= end and row["amount"] and row["volume"] and row["settlement"]
            ]
        day_col, _, _, _, _, settle_c, vol_c, _, _, _, amt_c = self._ensure_cols()
        lo = bisect_left(day_col, begin)
        out = []
        scale = Decimal(R) * Decimal(R)
        for i in range(lo, len(day_col)):
            if day_col[i] > end:
                break
            amt, vol, settle = amt_c[i], vol_c[i], settle_c[i]
            if amt and vol and settle:
                out.append(Decimal(amt) * scale / Decimal(vol) / Decimal(settle))
        return out


class Data:
    """One version of the research dataset, read per exchange and product."""

    def __init__(self, warehouse: Dataset, begin: str, end: str, *, version: str = ""):
        self.warehouse = warehouse
        self.version = warehouse.version(version) if version else warehouse.current()
        self.begin, self.end = begin, end
        first, last = datetime.date.fromisoformat(begin), datetime.date.fromisoformat(end)
        window = datetime.timedelta(days=92)
        self.daily_begin = (first - min(window, first - datetime.date.min)).isoformat()
        self.daily_end = (last + min(window, datetime.date.max - last)).isoformat()
        self.calendars, self.checked = {}, set()
        self.lives, self.units, self.multipliers, self.ticks, self.settlements = {}, {}, {}, {}, {}
        self.products, self.loaded_exchanges, self.pit_cache = {}, set(), {}
        self._exchange_active: dict[str, list[str]] = {}

    def pit_fundamentals(self, exchange: str) -> pit.ExchangePit:
        if exchange not in self.pit_cache:
            self.pit_cache[exchange] = pit.load_exchange_pit(
                self.warehouse, self.version, exchange, self.begin, self.end
            )
        return self.pit_cache[exchange]

    def calendar(self, exchange: str) -> dict:
        if exchange not in self.calendars:
            df = (
                self.warehouse.scan(self.version, "calendar", exchange=exchange)
                .filter(pl.col("exchange") == exchange)
                .select(pl.col("day").dt.strftime("%Y-%m-%d").alias("day"), pl.col("open"))
                .sort("day")
                .collect()
            )
            self.calendars[exchange] = dict(zip(df["day"].to_list(), df["open"].to_list()))
        return self.calendars[exchange]

    def check_stored(self, exchange: str):
        if exchange in self.checked:
            return
        calendar = self.calendar(exchange)
        opened = {day for day, is_open in calendar.items() if self.begin <= day <= self.end and is_open}
        if not opened:
            raise ValueError(f"the calendar of {exchange} is not stored for the range")
        missing = sorted(opened - self.version.days(exchange))
        if missing:
            raise ValueError(f"daily bars of {exchange} are not stored on {', '.join(missing[:5])}")
        self.checked.add(exchange)

    def _scan_daily(self, exchange: str, product: str | None = None) -> pl.DataFrame:
        begin_d = datetime.date.fromisoformat(self.daily_begin)
        end_d = datetime.date.fromisoformat(self.daily_end)
        lf = self.warehouse.scan(
            self.version, "daily", exchange=exchange, begin=self.daily_begin, end=self.daily_end
        ).filter((pl.col("exchange") == exchange) & pl.col("day").is_between(begin_d, end_d))
        if product is not None:
            lf = lf.filter(pl.col("product") == product)
        key_cols = ["product", "contract"] if product is None else ["contract"]
        return (
            lf.select(
                *key_cols,
                pl.col("day").dt.strftime("%Y-%m-%d").alias("day"),
                *((pl.col(c) * R).cast(pl.Int64).alias(c) for c in _DAILY_RAW_COLS),
                pl.col("amount"),
            )
            .sort([*key_cols, "day"])
            .collect()
        )

    def load_exchange(self, exchange: str) -> list:
        """All products of `exchange` around the range in one scan, returning
        those with daily bars inside [begin, end]."""
        if exchange not in self.loaded_exchanges:
            df = self._scan_daily(exchange)
            by_product: dict[str, dict[str, Daily]] = {}
            if not df.is_empty():
                active = (
                    df.filter(pl.col("day").is_between(pl.lit(self.begin), pl.lit(self.end)))["product"]
                    .unique()
                    .sort()
                    .to_list()
                )
                for (prod, contract), sub in df.partition_by(
                    ["product", "contract"], as_dict=True, maintain_order=True
                ).items():
                    by_product.setdefault(prod, {})[contract] = Daily(contract, frame=sub)
            else:
                active = []
            for prod, months in by_product.items():
                self.products[exchange, prod] = months
            self._exchange_active[exchange] = active
            self.loaded_exchanges.add(exchange)
        return list(self._exchange_active[exchange])

    def product(self, exchange: str, product: str) -> dict:
        """Every month of a product over the range and the windows around it."""
        key = (exchange, product)
        if key not in self.products:
            if exchange in self.loaded_exchanges:
                self.products[key] = {}
                return {}
            df = self._scan_daily(exchange, product)
            months = {}
            if not df.is_empty():
                for (contract,), sub in df.partition_by(
                    ["contract"], as_dict=True, maintain_order=True
                ).items():
                    months[contract] = Daily(contract, frame=sub)
            self.products[key] = months
        return self.products[key]

    def minute_frame(self, contract: str, begin: str, end: str) -> pl.DataFrame:
        begin_d, end_d = datetime.date.fromisoformat(begin), datetime.date.fromisoformat(end)
        return (
            self.warehouse.scan(
                self.version, "minutes", contract=contract, begin=begin, end=end
            )
            .filter((pl.col("contract") == contract) & pl.col("day").is_between(begin_d, end_d))
            .select(
                pl.col("day").dt.strftime("%Y-%m-%d").alias("day"),
                pl.col("start_ms"),
                pl.col("partial"),
                pl.col("open"),
                pl.col("high"),
                pl.col("low"),
                pl.col("close"),
                pl.col("volume"),
                pl.col("open_interest"),
            )
            .sort("start_ms")
            .collect()
        )

    def minute_raw_frame(self, contract: str, begin: str, end: str) -> pl.DataFrame:
        begin_d, end_d = datetime.date.fromisoformat(begin), datetime.date.fromisoformat(end)
        return (
            self.warehouse.scan(
                self.version, "minutes", contract=contract, begin=begin, end=end
            )
            .filter((pl.col("contract") == contract) & pl.col("day").is_between(begin_d, end_d))
            .select(
                pl.col("day").dt.strftime("%Y-%m-%d").alias("day"),
                pl.col("start_ms"),
                pl.col("partial"),
                (pl.col("open") * R).cast(pl.Int64).alias("open"),
                (pl.col("high") * R).cast(pl.Int64).alias("high"),
                (pl.col("low") * R).cast(pl.Int64).alias("low"),
                (pl.col("close") * R).cast(pl.Int64).alias("close"),
                (pl.col("volume") * R).cast(pl.Int64).alias("volume"),
                pl.col("open_interest").cast(pl.Float64).alias("open_interest"),
            )
            .sort("start_ms")
            .collect()
        )

    def minute_rows(self, contract: str, begin: str, end: str):
        return self.minute_frame(contract, begin, end).iter_rows()

    def contract_life(self, exchange: str, contract: str) -> tuple[datetime.date, datetime.date] | None:
        if exchange not in self.lives:
            df = (
                self.warehouse.scan(self.version, "contracts", exchange=exchange)
                .select("contract", "list_date", "delist_date")
                .unique()
                .collect()
            )
            lives = {}
            for name, listed, delisted in df.iter_rows():
                lives.setdefault(name, []).append((listed, delisted))
            self.lives[exchange] = lives
        rows = self.lives[exchange].get(contract, ())
        return rows[0] if len(rows) == 1 and None not in rows[0] else None

    def contract_days(self, exchange: str, life: tuple, begin: str, end: str) -> list:
        listed, delisted = life
        begin = max(datetime.date.fromisoformat(begin), listed)
        end = min(datetime.date.fromisoformat(end), delisted)
        calendar = self.calendar(exchange)
        days = [(begin + datetime.timedelta(days=n)).isoformat() for n in range((end - begin).days + 1)]
        missing = [day for day in days if calendar.get(day) is None]
        if missing:
            raise ValueError(f"the calendar of {exchange} is incomplete on {spans(missing, days)}")
        return [day for day in days if calendar[day]]

    def price_increments(self, exchange: str, contract: str, days: list) -> dict:
        if exchange not in self.ticks:
            df = (
                self.warehouse.scan(self.version, "price_ticks", exchange=exchange)
                .filter(pl.col("exchange") == exchange)
                .select("product", "contract", "effective_from", "effective_until", "price_tick")
                .collect()
            )
            self.ticks[exchange] = list(df.iter_rows())
        product = contract.split("/")[1]
        values = {day: set() for day in days}
        for item_product, item_contract, begin, end, tick in self.ticks[exchange]:
            if item_contract != contract and not (item_contract is None and item_product == product):
                continue
            if begin is None or end is None or begin >= end:
                raise ValueError(f"invalid price tick interval of {contract} on {begin or days[0]}")
            covered = days[bisect_left(days, begin.isoformat()) : bisect_left(days, end.isoformat())]
            if not covered:
                continue
            if tick is None or tick <= 0:
                raise ValueError(f"invalid price tick of {contract} on {covered[0]}")
            tick_raw = raw(tick)
            for day in covered:
                values[day].add(tick_raw)
        result = {}
        for day, ticks in values.items():
            if len(ticks) != 1:
                problem = "missing" if not ticks else "conflicting"
                raise ValueError(f"{problem} price tick of {contract} on {day}")
            result[day] = next(iter(ticks))
        return result

    def costs(
        self,
        exchange: str,
        contract: str,
        days: list,
        fee_multiplier: Decimal = Decimal(1),
    ) -> list:
        """The contract's exchange margin and fees on each of `days` (ascending), from the
        version's settlement parameters, as the engine's dated costs. A fee the exchange
        published for no source stops the run instead of being replaced."""
        if self.daily_begin <= days[0] and days[-1] <= self.daily_end:
            if exchange not in self.settlements:
                begin_d = datetime.date.fromisoformat(self.daily_begin)
                end_d = datetime.date.fromisoformat(self.daily_end)
                df = (
                    self.warehouse.scan(
                        self.version,
                        "settlement",
                        exchange=exchange,
                        begin=self.daily_begin,
                        end=self.daily_end,
                    )
                    .filter((pl.col("exchange") == exchange) & pl.col("day").is_between(begin_d, end_d))
                    .select(
                        "contract",
                        pl.col("day").dt.strftime("%Y-%m-%d").alias("day"),
                        *((pl.col(c) * R).cast(pl.Int64).alias(c) for c in _SETTLEMENT_RAW_COLS),
                    )
                    .collect()
                )
                by_contract: dict[str, dict[str, tuple]] = {}
                if not df.is_empty():
                    for item_contract, day, *values in df.iter_rows():
                        by_contract.setdefault(item_contract, {})[day] = tuple(values)
                self.settlements[exchange] = by_contract
            published = self.settlements[exchange].get(contract, {})
        else:
            df = (
                self.warehouse.scan(
                    self.version, "settlement", exchange=exchange, begin=days[0], end=days[-1]
                )
                .filter(pl.col("contract") == contract)
                .select(
                    pl.col("day").dt.strftime("%Y-%m-%d").alias("day"),
                    *((pl.col(c) * R).cast(pl.Int64).alias(c) for c in _SETTLEMENT_RAW_COLS),
                )
                .collect()
            )
            published = {day: tuple(values) for day, *values in df.iter_rows()}
        schedule = []
        for day in days:
            values = published.get(day)
            if values is None:
                raise ValueError(f"no settlement parameters of {contract} on {day}")
            long_r, short_r, rate_r, per_lot_r, today_rate_r, today_per_lot_r = values
            if None in (long_r, short_r, rate_r, per_lot_r):
                raise ValueError(f"missing fees of {contract} on {day}")
            has_today = today_rate_r is not None and today_per_lot_r is not None
            # As COSTS names them, plus short_margin_rate: closing yesterday costs what opening does;
            # missing close-today fees stay None and fail only if a close-today trade occurs.
            # fee_multiplier scales commission fees only; margin rates stay exact exchange rates.
            fee_per = _scale_fee_raw(per_lot_r, fee_multiplier)
            fee_rate = _scale_fee_raw(rate_r, fee_multiplier)
            costs = [
                0,
                fee_per,
                _scale_fee_raw(today_per_lot_r, fee_multiplier) if has_today else None,
                fee_per,
                long_r,
                fee_rate,
                _scale_fee_raw(today_rate_r, fee_multiplier) if has_today else None,
                fee_rate,
                short_r,
            ]
            if not schedule or schedule[-1]["values"] != costs:
                schedule.append(dict(effective_from=day, source="warehouse", values=costs))
        return schedule

    def multiplier(self, exchange: str, contract: str) -> Decimal:
        if contract not in self.multipliers:
            self.multipliers[contract] = self.read_multiplier(exchange, contract)
        return self.multipliers[contract]

    def read_multiplier(self, exchange: str, contract: str) -> Decimal:
        product = contract.split("/")[1]
        if exchange not in self.units:
            df = (
                self.warehouse.scan(self.version, "contracts", exchange=exchange)
                .filter(pl.col("exchange") == exchange)
                .select("contract", "multiplier", "per_unit", "trade_unit", "quote_unit")
                .unique()
                .collect()
            )
            units = {}
            for name, *row in df.iter_rows():
                units.setdefault(name, []).append(tuple(row))
            self.units[exchange] = units
        rows = self.units[exchange].get(contract)
        if not rows:
            raise ValueError(f"the contract table has no {contract}")
        if len(rows) > 1:
            raise ValueError(f"the contract table's units of {contract} differ between snapshots")
        scale, per_unit, trade_unit, quote_unit = rows[0]
        lots = quoted_lots(trade_unit, quote_unit)
        if per_unit is None or per_unit <= 0 or lots is None:
            raise ValueError(f"the contract table's units of {contract} cannot be read")
        result = (Decimal(1) if scale is None else Decimal(scale)) * Decimal(per_unit) * lots
        # Turnover of a delivery month includes lots that changed hands away from the
        # day's prices, so a day agrees within a twentieth, and most days must.
        implied = self.product(exchange, product)[contract].implied_multipliers(self.begin, self.end)
        agree = sum(abs(value / result - 1) <= Decimal("0.05") for value in implied)
        if agree * 2 < len(implied):
            median = sorted(implied)[len(implied) // 2]
            raise ValueError(
                f"the contract multiplier does not match the turnover of {contract}: "
                f"its days imply about {float(median):g}"
            )
        return result


def quoted_lots(trade_unit, quote_unit):
    """A lot in units of what the price is quoted per: one where the price is per
    the unit traded in or per no unit, two where it is per 500 kg of tonnes."""
    if not trade_unit or not quote_unit:
        return None
    if "/" not in quote_unit:
        return Decimal(1)
    per = quote_unit.split("/", 1)[1]
    if per == trade_unit:
        return Decimal(1)
    if per == "500千克" and trade_unit == "吨":
        return Decimal(2)
    return None


def spans(missing: list, days: list) -> str:
    """`missing` days as runs of consecutive `days`."""
    runs = []
    for day in missing:
        if runs and days.index(day) == days.index(runs[-1][-1]) + 1:
            runs[-1].append(day)
        else:
            runs.append([day])
    return ", ".join(
        run[0] if len(run) == 1 else f"{run[0]} to {run[-1]} ({len(run)} days)" for run in runs
    )


def contract_input(
    data: Data, config, contract: str, begin: str, end: str, prices_until: str = ""
) -> dict:
    """After handover, missing prices matter only while the outgoing month is held."""
    exchange, product, _ = contract.split("/")
    month = data.product(exchange, product).get(contract)
    life = data.contract_life(exchange, contract)
    if life is None:
        raise ValueError(f"the contract table has no unique listed life of {contract}")
    days = data.contract_days(exchange, life, begin, end)
    rows_by_day = {day: month.row_raw(day) for day in days} if month is not None else {}
    missing = [day for day in days if rows_by_day.get(day) is None]
    if missing:
        raise ValueError(f"daily bars of {contract} are not stored on {spans(missing, days)}")
    unsettled = [day for day in days if rows_by_day[day][5] is None]
    if unsettled:
        raise ValueError(f"settlement prices of {contract} are missing on {spans(unsettled, days)}")
    unpriced = []
    if not config.minutes:
        missing = [
            day
            for day in days
            if rows_by_day[day][6] != 0
            and None in rows_by_day[day][1:5]
        ]
        unpriced = [day for day in missing if day > (prices_until or end)]
        missing = [day for day in missing if day <= (prices_until or end)]
        if missing:
            raise ValueError(f"daily prices of {contract} are missing on {spans(missing, days)}")
        unpriced_set = set(unpriced)
        days = [
            day
            for day in days
            if day not in unpriced_set and rows_by_day[day][0] and rows_by_day[day][4] is not None
        ]
    if not days:
        raise ValueError(f"no bars of {contract} fall inside the requested trading days")

    excluded = []
    coverage = dict(contract=contract, missing={}, zero_volume_minutes=0, unchecked_days=[])
    if config.minutes:
        bars, stored, excluded, coverage = chart.minute_coverage_raw(
            data.minute_raw_frame(contract, begin, end),
            days,
            config.minutes,
            contract,
            data.calendar(exchange),
            life[0],
        )
        missing = [day for day in days if day not in stored and rows_by_day[day][6] != 0]
        unpriced = [day for day in missing if day > (prices_until or end)]
        missing = [day for day in missing if day <= (prices_until or end)]
        if missing:
            raise ValueError(f"minutes of {contract} are not stored on {spans(missing, days)}")
        if not bars:
            raise ValueError(f"no stored minutes of {contract} fall inside the requested trading days")
    else:
        bars = [
            (
                day,
                close_ns(day),
                rows_by_day[day][1],
                rows_by_day[day][2],
                rows_by_day[day][3],
                rows_by_day[day][4],
                rows_by_day[day][6],
                rows_by_day[day][7],
            )
            for day in days
        ]
    settlements = month.settlements_raw(begin, end)
    if config.costs is None:
        tick_days = sorted({bar[0] for bar in bars} | {day for day, _ in settlements if day >= bars[0][0]})
        increments = data.price_increments(exchange, contract, tick_days)
        initial = current = increments[bars[0][0]]
        changes = []
        for day in tick_days[1:]:
            if increments[day] != current:
                current = increments[day]
                changes.append((day, current))
        costs = data.costs(exchange, contract, tick_days, config.fee_multiplier)
    else:
        # A replay that trades nothing prices every contract from its first day on.
        initial, changes = 1, []
        costs = [
            dict(
                effective_from=config.begin_day,
                source="replay",
                values=[raw(value) for value in config.costs],
            )
        ]
    price_limits = []
    for day in sorted({bar[0] for bar in bars}):
        row = rows_by_day[day]
        lower, upper = row[8], row[9]
        if lower is None and upper is None:
            continue
        if lower is None or upper is None:
            raise ValueError(f"incomplete price limits of {contract} on {day}")
        tick = increments[day] if config.costs is None else 1
        price_limits.append((day, _limit_units_raw(lower, tick), _limit_units_raw(upper, tick)))
    pit_exchange = data.pit_fundamentals(exchange)
    pit_map = {}
    if not pit_exchange.empty():
        for bar in bars:
            vals = pit_exchange.lookup(product, bar[0], bar[1] // 10**6)
            if any(math.isfinite(v) for v in vals):
                pit_map[bar[1]] = vals
    return dict(
        venue=exchange,
        symbol=contract,
        price_increment=initial,
        quantity_increment=R,
        multiplier=raw(data.multiplier(exchange, contract)),
        costs=costs,
        increments=changes,
        slippage_ticks=config.slippage_ticks,
        price_limits=price_limits,
        bars=[bar[:7] for bar in bars],
        settlements=settlements,
        # Not the engine's: what a strategy reads besides prices, by bar time,
        # and the trading day of each bar left out.
        open_interest={bar[1]: bar[7] for bar in bars},
        pit=pit_map,
        excluded=excluded,
        unpriced=unpriced,
        minute_coverage=coverage,
    )


def series_input(data: Data, config, exchange: str, product: str, *, leading_only: bool = False):
    """The months of a product's traded dominant series and its rolls."""
    data.check_stored(exchange)
    product_months = data.product(exchange, product)
    anchor = max(
        (
            found
            for month in product_months.values()
            if (found := month.anchor_before(config.begin_day)) is not None
        ),
        default=config.begin_day,
    )
    traded = {}
    for contract, month in product_months.items():
        bars = month.bars(config.begin_day, config.end_day)
        if bars:
            traded[contract] = bars
    if len(traded) < 2:
        raise ValueError("a dominant series requires at least two month contracts")
    months = []
    for contract in sorted(traded):
        life = data.contract_life(exchange, contract)
        settlement_map, oi_map = product_months[contract].settlement_and_oi_raw(anchor, config.end_day)
        months.append(
            dominant.Month(
                contract,
                contract.rsplit("/", 1)[1],
                settlement=settlement_map,
                open_interest=oi_map,
                last_bar=traded[contract][-1],
                delist_date=life[1].isoformat() if life else "",
            )
        )
    plan = dominant.plan(months, traded=True, end=config.end_day)
    # Precompute previous auction close only for months used as dominant or next-dominant.
    used_contracts = {months[seg.month].contract for seg in plan.segments} | {
        months[sub_idx].contract for sub_idx in plan.second.values()
    }
    prev_daily_close = {
        c_name: product_months[c_name].prev_auction_closes_raw() for c_name in used_contracts
    }
    sub_minute_closes = {}
    sub_minute_prev = {}
    if config.minutes:
        needed_days = {}
        for day, sub_idx in plan.second.items():
            if config.begin_day <= day <= config.end_day:
                needed_days.setdefault(months[sub_idx].contract, set()).add(day)
        for sub_contract, days_set in needed_days.items():
            _, sub_prod, sub_month = sub_contract.split("/")
            stored = sorted(
                d for d in days_set if data.version.has(f"minutes/{exchange}/{d}/{sub_prod}/{sub_month}")
            )
            if not stored:
                continue
            closes, prevs, last_sub = {}, {}, None
            raw_frame = data.minute_raw_frame(sub_contract, stored[0], stored[-1])
            if not raw_frame.is_empty():
                raw_frame = raw_frame.filter(pl.col("day").is_in(stored))
            _, bar_end_ms, bar_close, bar_partial = chart.minute_bars_raw(
                raw_frame, config.minutes, sub_contract
            )
            for end_ms, c_val, is_part in zip(
                bar_end_ms.tolist(), bar_close.tolist(), bar_partial.tolist()
            ):
                if not is_part:
                    t_ns = end_ms * 10**6
                    closes[t_ns] = c_val
                    if last_sub is not None and last_sub > 0:
                        prevs[t_ns] = last_sub
                    last_sub = c_val
            sub_minute_closes[sub_contract] = closes
            sub_minute_prev[sub_contract] = prevs
    contracts, rolls = [], []
    for i, segment in enumerate(plan.segments):
        month = months[segment.month]
        # A paused strategy can keep the old month until it gives a new target.
        begin = segment.before if i else segment.begin
        next_begin = plan.segments[i + 1].begin if i + 1 < len(plan.segments) else ""
        # A month that hands over trades through its bar on or after the roll day, and the next.
        until = ""
        if next_begin:
            bars = traded[month.contract]
            closed = next(n for n, day in enumerate(bars) if day >= next_begin)
            until = bars[min(closed + 1, len(bars) - 1)]
        contract_end = until if (leading_only and until) else config.end_day
        entry = contract_input(data, config, month.contract, begin, contract_end, until)
        entry["roll_ratio"] = float(plan.ratios[i])
        ry_map, far_close_map, near_ret_map, far_ret_map = {}, {}, {}, {}
        prev_near_min = None
        near_prev_daily = prev_daily_close.get(month.contract, {})
        for day, time_ns, _, _, _, close_raw, _ in entry["bars"]:
            near_close = close_raw if close_raw > 0 else None
            if config.minutes:
                prev_near = prev_near_min
                if near_close is not None:
                    prev_near_min = near_close
            else:
                prev_near = near_prev_daily.get(day)
            if day < segment.begin or (next_begin and day >= next_begin):
                continue
            if near_close is not None and prev_near is not None:
                n_ret = dominant.simple_return(near_close, prev_near)
                if n_ret is not None:
                    near_ret_map[time_ns] = float(n_ret)
            sub_idx = plan.second.get(day)
            if sub_idx is None or near_close is None:
                continue
            sub_month = months[sub_idx]
            if config.minutes:
                sub_close = sub_minute_closes.get(sub_month.contract, {}).get(time_ns)
                prev_sub = sub_minute_prev.get(sub_month.contract, {}).get(time_ns)
            else:
                sub_close = product_months[sub_month.contract].auction_close_raw(day)
                prev_sub = prev_daily_close.get(sub_month.contract, {}).get(day)
            if sub_close is not None and sub_close > 0:
                far_close_map[time_ns] = sub_close / R
                ry_map[time_ns] = float(
                    dominant.annualized_roll_yield(
                        near_close, sub_close, month.delivery_month, sub_month.delivery_month
                    )
                )
                if prev_sub is not None:
                    f_ret = dominant.simple_return(sub_close, prev_sub)
                    if f_ret is not None:
                        far_ret_map[time_ns] = float(f_ret)
        entry["roll_yield"] = ry_map
        entry["far_close"] = far_close_map
        entry["near_return"] = near_ret_map
        entry["far_return"] = far_ret_map
        contracts.append(entry)
        rolls.append((segment.begin, i))
    return contracts, rolls


def prepare(data: Data, config, *, leading_only: bool = False):
    """The engine's contracts and series, and for each symbol the index of its
    first contract in them. Each contract is marked with the symbol it trades for."""
    contracts, series, first = [], [], {}
    with data.warehouse:
        counts = {}
        for symbol in config.symbols:
            parts = symbol.split("/")
            if len(parts) in (2, 3):
                counts.setdefault(parts[0], set()).add(parts[1])
        for exchange, products in counts.items():
            if len(products) > 1:
                data.load_exchange(exchange)
        for symbol in config.symbols:
            parts = symbol.split("/")
            first[symbol] = start = len(contracts)
            if len(parts) == 3:
                contracts.append(contract_input(data, config, symbol, config.begin_day, config.end_day))
            elif len(parts) == 2:
                months, rolls = series_input(data, config, *parts, leading_only=leading_only)
                offset = len(contracts)
                contracts.extend(months)
                series.append([(day, offset + index) for day, index in rolls])
            else:
                raise ValueError(f"a symbol is EXCHANGE/product/YYYY-MM or EXCHANGE/product: {symbol}")
            for item in contracts[start:]:
                item["run_symbol"] = symbol
    return contracts, series, first


def open_data(directory: Path, config) -> Data:
    """The admitted version of the research dataset."""
    if not config.dataset_version:
        raise ValueError("research inputs must name the admitted dataset version")
    return Data(Dataset(directory), config.begin_day, config.end_day, version=config.dataset_version)


def stored_span(found: dict, symbols, minutes: bool) -> tuple[str, str]:
    table = "minutes" if minutes else "daily"
    firsts, lasts = [], []
    for symbol in symbols:
        matched = [
            found[c][table]
            for c in found
            if table in found[c]
            and (c == symbol if symbol.count("/") == 2 else c.startswith(f"{symbol}/"))
        ]
        if not matched:
            raise ValueError(f"no stored {table} bars for {symbol}")
        firsts.append(min(item[0] for item in matched))
        lasts.append(max(item[1] for item in matched))
    overlap_first, overlap_last = max(firsts), min(lasts)
    if overlap_first <= overlap_last:
        return overlap_first, overlap_last
    return min(firsts), max(lasts)
