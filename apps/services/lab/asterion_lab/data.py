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
from dataclasses import dataclass
import datetime
from decimal import Decimal, ROUND_HALF_UP
import functools
from itertools import groupby
import math
from operator import itemgetter
from pathlib import Path

from asterion_lab import chart, dominant, pit
from asterion_lab.warehouse_client import Dataset

R = 10**8
BEIJING = datetime.timezone(datetime.timedelta(hours=8))
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


def scale_fee(value, multiplier: Decimal) -> int:
    """Scale an exchange commission fee by `multiplier`, rounded to engine precision."""
    scaled = (Decimal(value) * multiplier).quantize(Decimal("0.00000001"), rounding=ROUND_HALF_UP)
    return raw(scaled)


def limit_units(value, tick: int) -> int:
    """Tushare stores 69 SHFE gold limits exactly 0.001 below their price tick."""
    units = raw(value)
    above = units + R // 1000
    return above if above % tick == 0 and units % tick else units


@functools.lru_cache(maxsize=8192)
def close_ns(day: str) -> int:
    # 15:00 Asia/Shanghai is 07:00 UTC.
    return int(datetime.datetime.fromisoformat(f"{day}T07:00:00+00:00").timestamp()) * 10**9


@dataclass
class Daily:
    """What one contract did on each day read: auction prices, settlement,
    volume, turnover and open interest."""

    contract: str
    days: dict  # day -> row

    def bars(self, begin: str, end: str) -> list:
        return [
            day
            for day, row in sorted(self.days.items())
            if begin <= day <= end and row["auction"] and row["settlement"] is not None
        ]


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

    def pit_fundamentals(self, exchange: str) -> pit.ExchangePit:
        if exchange not in self.pit_cache:
            self.pit_cache[exchange] = pit.load_exchange_pit(
                self.warehouse, self.version, exchange, self.begin, self.end
            )
        return self.pit_cache[exchange]

    def calendar(self, exchange: str) -> dict:
        if exchange not in self.calendars:
            rows = self.warehouse.query(
                self.version,
                """
                select day, open from calendar where exchange = ? order by day""",
                ("calendar",),
                (exchange,),
                exchange=exchange,
            )
            self.calendars[exchange] = {day.isoformat(): opened for day, opened in rows}
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

    def load_exchange(self, exchange: str) -> list:
        """All products of `exchange` around the range in one scan, returning
        those with daily bars inside [begin, end]."""
        if exchange not in self.loaded_exchanges:
            rows = self.warehouse.query(
                self.version,
                """
                select product, contract, day, open, high, low, close, settlement,
                       volume, amount, open_interest, lower_limit, upper_limit from daily
                where exchange = ? and day between ?::date and ?::date
                order by product, contract, day""",
                ("daily",),
                (exchange, self.daily_begin, self.daily_end),
                exchange=exchange,
                begin=self.daily_begin,
                end=self.daily_end,
            )
            by_product = {}
            for product, contract, day, *values in rows:
                open_, high, low, close, settlement, volume, amount, interest, lower, upper = values
                months = by_product.setdefault(product, {})
                months.setdefault(contract, Daily(contract, {}))
                months[contract].days[day.isoformat()] = dict(
                    auction=None not in (open_, high, low),
                    open=open_,
                    high=high,
                    low=low,
                    close=close,
                    settlement=settlement,
                    volume=volume,
                    amount=amount,
                    open_interest=interest,
                    lower_limit=lower,
                    upper_limit=upper,
                )
            for product, months in by_product.items():
                self.products[exchange, product] = months
            self.loaded_exchanges.add(exchange)
        return sorted(
            product
            for (ex, product), months in self.products.items()
            if ex == exchange
            and any(self.begin <= day <= self.end for month in months.values() for day in month.days)
        )

    def product(self, exchange: str, product: str) -> dict:
        """Every month of a product over the range and the windows around it."""
        key = (exchange, product)
        if key not in self.products:
            if exchange in self.loaded_exchanges:
                self.products[key] = {}
                return {}
            rows = self.warehouse.query(
                self.version,
                """
                select contract, day, open, high, low, close, settlement, volume, amount,
                       open_interest, lower_limit, upper_limit from daily
                where exchange = ? and product = ? and day between ?::date and ?::date
                order by contract, day""",
                ("daily",),
                (exchange, product, self.daily_begin, self.daily_end),
                exchange=exchange,
                begin=self.daily_begin,
                end=self.daily_end,
            )
            months = {}
            for contract, day, *values in rows:
                open_, high, low, close, settlement, volume, amount, interest, lower, upper = values
                months.setdefault(contract, Daily(contract, {}))
                months[contract].days[day.isoformat()] = dict(
                    auction=None not in (open_, high, low),
                    open=open_,
                    high=high,
                    low=low,
                    close=close,
                    settlement=settlement,
                    volume=volume,
                    amount=amount,
                    open_interest=interest,
                    lower_limit=lower,
                    upper_limit=upper,
                )
            self.products[key] = months
        return self.products[key]

    def minute_rows(self, contract: str, begin: str, end: str):
        rows = self.warehouse.query(
            self.version,
            """
            select day, start_ms, partial, open, high, low, close, volume, open_interest
            from minutes where contract = ? and day between ?::date and ?::date
            order by start_ms""",
            ("minutes",),
            (contract, begin, end),
            contract=contract,
            begin=begin,
            end=end,
        )
        return ((day.isoformat(), *values) for day, *values in rows)

    def contract_life(self, exchange: str, contract: str) -> tuple[datetime.date, datetime.date] | None:
        if exchange not in self.lives:
            lives = {}
            for name, listed, delisted in self.warehouse.query(
                self.version,
                """
                    select distinct contract, list_date, delist_date from contracts""",
                ("contracts",),
                exchange=exchange,
            ):
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
            self.ticks[exchange] = self.warehouse.query(
                self.version,
                """
                select product, contract, effective_from, effective_until, price_tick
                from price_ticks where exchange = ?""",
                ("price_ticks",),
                (exchange,),
                exchange=exchange,
            )
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
            for day in covered:
                values[day].add(raw(tick))
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
                rows = self.warehouse.query(
                    self.version,
                    """
                    select contract, day, long_margin_rate, short_margin_rate, fee_rate, fee_per_lot,
                           close_today_fee_rate, close_today_fee_per_lot
                    from settlement where exchange = ? and day between ?::date and ?::date""",
                    ("settlement",),
                    (exchange, self.daily_begin, self.daily_end),
                    exchange=exchange,
                    begin=self.daily_begin,
                    end=self.daily_end,
                )
                by_contract = {}
                for item_contract, day, *values in rows:
                    by_contract.setdefault(item_contract, {})[day.isoformat()] = tuple(values)
                self.settlements[exchange] = by_contract
            published = self.settlements[exchange].get(contract, {})
        else:
            rows = self.warehouse.query(
                self.version,
                """
                select day, long_margin_rate, short_margin_rate, fee_rate, fee_per_lot,
                       close_today_fee_rate, close_today_fee_per_lot
                from settlement where contract = ?""",
                ("settlement",),
                (contract,),
                exchange=exchange,
                begin=days[0],
                end=days[-1],
            )
            published = {day.isoformat(): values for day, *values in rows}
        schedule = []
        for day in days:
            values = published.get(day)
            if values is None:
                raise ValueError(f"no settlement parameters of {contract} on {day}")
            long, short, rate, per_lot, today_rate, today_per_lot = values
            if None in (long, short, rate, per_lot):
                raise ValueError(f"missing fees of {contract} on {day}")
            has_today = today_rate is not None and today_per_lot is not None
            # As COSTS names them, plus short_margin_rate: closing yesterday costs what opening does;
            # missing close-today fees stay None and fail only if a close-today trade occurs.
            # fee_multiplier scales commission fees only; margin rates stay exact exchange rates.
            costs = [
                raw(Decimal(0)),
                scale_fee(per_lot, fee_multiplier),
                scale_fee(today_per_lot, fee_multiplier) if has_today else None,
                scale_fee(per_lot, fee_multiplier),
                raw(long),
                scale_fee(rate, fee_multiplier),
                scale_fee(today_rate, fee_multiplier) if has_today else None,
                scale_fee(rate, fee_multiplier),
                raw(short),
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
            units = {}
            for name, *row in self.warehouse.query(
                self.version,
                """
                    select distinct contract, multiplier, per_unit, trade_unit, quote_unit
                    from contracts where exchange = ?""",
                ("contracts",),
                (exchange,),
                exchange=exchange,
            ):
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
        implied = [
            Decimal(row["amount"]) / Decimal(row["volume"]) / Decimal(row["settlement"])
            for day, row in self.product(exchange, product)[contract].days.items()
            if self.begin <= day <= self.end and row["amount"] and row["volume"] and row["settlement"]
        ]
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
    missing = [day for day in days if month is None or day not in month.days]
    if missing:
        raise ValueError(f"daily bars of {contract} are not stored on {spans(missing, days)}")
    unsettled = [day for day in days if month.days[day]["settlement"] is None]
    if unsettled:
        raise ValueError(f"settlement prices of {contract} are missing on {spans(unsettled, days)}")
    unpriced = []
    if not config.minutes:
        missing = [
            day
            for day in days
            if month.days[day]["volume"] != 0
            and any(month.days[day][field] is None for field in ("open", "high", "low", "close"))
        ]
        unpriced = [day for day in missing if day > (prices_until or end)]
        missing = [day for day in missing if day <= (prices_until or end)]
        if missing:
            raise ValueError(f"daily prices of {contract} are missing on {spans(missing, days)}")
        days = [
            day
            for day in days
            if day not in unpriced and month.days[day]["auction"] and month.days[day]["close"] is not None
        ]
    if not days:
        raise ValueError(f"no bars of {contract} fall inside the requested trading days")

    def interest(value):
        return math.nan if value is None else float(value)

    excluded = []
    coverage = dict(contract=contract, missing={}, zero_volume_minutes=0, unchecked_days=[])
    if config.minutes:
        settled = set(days)
        minutes = []
        for day, group in groupby(data.minute_rows(contract, begin, end), key=itemgetter(0)):
            if day not in settled:
                continue
            priced, observed = chart.minute_coverage(
                day, list(group), config.minutes, contract, data.calendar(exchange), life[0]
            )
            minutes.extend(priced)
            excluded.extend([day] * len(observed["partial"]))
            if observed["missing"]:
                coverage["missing"][day] = len(observed["missing"])
            coverage["zero_volume_minutes"] += observed["zero_volume_minutes"]
            if observed["unchecked"]:
                coverage["unchecked_days"].append(day)
        stored = {bar["trading_day"] for bar in minutes}
        missing = [day for day in days if day not in stored and month.days[day]["volume"] != 0]
        unpriced = [day for day in missing if day > (prices_until or end)]
        missing = [day for day in missing if day <= (prices_until or end)]
        if missing:
            raise ValueError(f"minutes of {contract} are not stored on {spans(missing, days)}")
        bars = [
            (
                bar["trading_day"],
                bar["time_ms"] * 10**6,
                *(raw(bar[name]) for name in ("open", "high", "low", "close", "volume")),
                interest(bar["open_interest"]),
            )
            for bar in minutes
            if not bar["partial"]
        ]
        if not bars:
            raise ValueError(f"no stored minutes of {contract} fall inside the requested trading days")
    else:
        bars = [
            (
                day,
                close_ns(day),
                *(raw(month.days[day][name]) for name in ("open", "high", "low", "close", "volume")),
                interest(month.days[day]["open_interest"]),
            )
            for day in days
        ]
    settlements = [
        (day, raw(row["settlement"]))
        for day, row in sorted(month.days.items())
        if begin <= day <= end and row["settlement"] is not None
    ]
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
        row = month.days[day]
        lower, upper = row["lower_limit"], row["upper_limit"]
        if lower is None and upper is None:
            continue
        if lower is None or upper is None:
            raise ValueError(f"incomplete price limits of {contract} on {day}")
        tick = increments[day] if config.costs is None else 1
        price_limits.append((day, limit_units(lower, tick), limit_units(upper, tick)))
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
            day
            for month in product_months.values()
            for day, row in month.days.items()
            if day < config.begin_day and row["settlement"] is not None
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
        item = dominant.Month(
            contract,
            contract.rsplit("/", 1)[1],
            last_bar=traded[contract][-1],
            delist_date=life[1].isoformat() if life else "",
        )
        for day, row in product_months[contract].days.items():
            if anchor <= day <= config.end_day:
                if row["settlement"] is not None:
                    item.settlement[day] = Decimal(row["settlement"])
                if row["open_interest"] is not None:
                    item.open_interest[day] = Decimal(row["open_interest"])
        months.append(item)
    plan = dominant.plan(months, traded=True, end=config.end_day)
    # Precompute previous auction close per contract so same-contract returns stay exact across rolls.
    prev_daily_close = {}
    for c_name, c_daily in product_months.items():
        prev_map, last_close = {}, None
        for d, row in sorted(c_daily.days.items()):
            if row["auction"] and row["close"] is not None:
                c_val = Decimal(row["close"])
                if last_close is not None and last_close > 0:
                    prev_map[d] = last_close
                last_close = c_val
        prev_daily_close[c_name] = prev_map
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
            stored_set = set(stored)
            for day, group in groupby(
                data.minute_rows(sub_contract, stored[0], stored[-1]), key=itemgetter(0)
            ):
                if day not in stored_set:
                    continue
                for bar in chart.minute_bars(list(group), config.minutes, sub_contract):
                    if not bar["partial"] and bar["close"] is not None:
                        t_ns = bar["time_ms"] * 10**6
                        c_val = Decimal(bar["close"])
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
        for day, time_ns, _, _, _, close_raw, _ in entry["bars"]:
            near_close = Decimal(close_raw).scaleb(-8) if close_raw > 0 else None
            if config.minutes:
                prev_near = prev_near_min
                if near_close is not None:
                    prev_near_min = near_close
            else:
                prev_near = prev_daily_close.get(month.contract, {}).get(day)
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
                sub_row = product_months[sub_month.contract].days.get(day)
                sub_close = (
                    Decimal(sub_row["close"])
                    if sub_row and sub_row["auction"] and sub_row["close"] is not None
                    else None
                )
                prev_sub = prev_daily_close.get(sub_month.contract, {}).get(day)
            if sub_close is not None and sub_close > 0:
                far_close_map[time_ns] = float(sub_close)
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
