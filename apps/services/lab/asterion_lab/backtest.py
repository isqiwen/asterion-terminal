"""Backtests: a strategy of the user's repository over the warehouse's daily
or minute bars, through asterion_engine (core's Replay).

The data are read this way:

- A contract has a daily bar when it traded in the auction (open,
  high and low present) and settled; the bar is labelled 15:00 Shanghai.
- Its multiplier is the contract table's, times the lot in the unit the
  price is quoted per; most days with turnover must agree with it within 5%.
- A contract's price increment comes from the pinned price-tick history;
  its effective intervals must cover each replayed trading day.
- A product's dominant series (symbol EXCHANGE/product) trades its dominant
  months by the rule of dominant.py; a later month joins a day before its
  roll. Months remain available through the requested range or delisting so
  paused positions can settle and roll after a new target. A strategy reads
  the series adjusted as of each bar (see asterion_strategy.py).

Margins and fees are the exchange's settlement parameters of each trading
day in the pinned dataset; a day without them stops the run. Slippage, order
limits and the weight budgets are the run's, as the user gives them. Weights
are sized into lots once, when given. Lots stand and what is left of them is
placed again at every bar's close, each order at most the order limit. A
strategy that is not ready places nothing and keeps what it holds; once ready
again, only the targets it then gives stand. Minute runs require daily settlements
on open days within the contract's listed life, and minutes unless daily volume is zero.

The service runs each backtest in a child process:

    python3 -I -B -c "...; from asterion_lab.backtest import main; main()" <lab-directory>

reads its AdmittedRun (asterion/lab.proto) from standard input.
"""
from bisect import bisect_left
from dataclasses import dataclass
import datetime
from decimal import Decimal
import math
import sys
from pathlib import Path

import polars as pl

from asterion_lab import chart, dominant, repository
from asterion_lab.runs import AMOUNT, TABLES, Plan, Runs
from asterion_strategy import Bar, Context, step
from asterion_lab.warehouse_client import Dataset

R = 10 ** 8
BEIJING = datetime.timezone(datetime.timedelta(hours=8))
PLACES = Decimal("0.00000001")
COSTS = ("margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee", "margin_rate",
         "open_fee_rate", "close_today_fee_rate", "close_yesterday_fee_rate")


def raw(value) -> int:
    """An exact decimal of at most eight places as the engine's integer."""
    value = Decimal(value)
    units = value * R
    if units != units.to_integral_value():
        raise ValueError(f"{value} has more than eight decimal places")
    return int(units)


def close_ns(day: str) -> int:
    # 15:00 Asia/Shanghai is 07:00 UTC.
    return int(datetime.datetime.fromisoformat(f"{day}T07:00:00+00:00").timestamp()) * 10 ** 9


@dataclass
class Config:
    repository: str
    strategy: str
    params: dict
    symbols: tuple
    begin_day: str
    end_day: str
    deposit: Decimal
    # None: each contract's exchange fees of each day from the dataset. A replay that
    # trades nothing gives eight decimals, as COSTS names them, for every contract.
    costs: list | None
    slippage_ticks: int
    max_order_lots: Decimal
    max_gross_lots: Decimal
    max_working_orders: int
    # What ctx.weights may ask for in all, gross and net, as shares of equity.
    max_gross_weight: Decimal = Decimal(0)
    max_net_weight: Decimal = Decimal(0)
    # Statistics: periods in a year, and an explicitly zero risk-free rate;
    # without them the annual figures and the Sharpe ratio stay absent.
    annual_periods: float | None = None
    zero_risk_free: bool = False
    # Days from this one on are the holdout: statistics are also kept for the
    # days before it and for it alone.
    holdout_day: str = ""
    # Bars of this many minutes, or daily bars when 0; settlements and rolls stay daily.
    minutes: int = 0
    dataset_version: str = ""
    # The copy of the repository's code taken at admission, and its revision.
    source: str = ""
    commit: str = ""
    changes: str = ""


@dataclass
class Daily:
    """What one contract did on each day read: auction prices, settlement,
    volume, turnover and open interest."""
    contract: str
    days: dict  # day -> row

    def bars(self, begin: str, end: str) -> list:
        return [day for day, row in sorted(self.days.items())
                if begin <= day <= end and row["auction"] and row["settlement"] is not None]


class Data:
    """One version of the research dataset, read per product."""

    def __init__(self, warehouse: Dataset, begin: str, end: str, *, version: str = ""):
        self.warehouse = warehouse
        self.version = warehouse.version(version) if version else warehouse.current()
        self.begin, self.end = begin, end
        self.products, self.units, self.multipliers = {}, {}, {}

    def check_stored(self, exchange: str):
        rows = self.warehouse.query(self.version, """
            select day from calendar where exchange = ? and open
            and day between ?::date and ?::date""", ("calendar",),
            (exchange, self.begin, self.end), exchange=exchange)
        if not rows:
            raise ValueError(f"the calendar of {exchange} is not stored for the range")
        missing = sorted({row[0].isoformat() for row in rows} - self.version.days(exchange))
        if missing:
            raise ValueError(f"daily bars of {exchange} are not stored on {', '.join(missing[:5])}")

    def product(self, exchange: str, product: str) -> dict:
        """Every month of a product over the range and the windows around it."""
        key = (exchange, product)
        if key not in self.products:
            self.check_stored(exchange)
            rows = self.warehouse.query(self.version, """
                select contract, day, open, high, low, close, settlement, volume, amount,
                       open_interest from daily
                where exchange = ? and product = ? and day between ?::date - 92 and ?::date + 92
                order by contract, day""", ("daily",),
                (exchange, product, self.begin, self.end), exchange=exchange)
            months = {}
            for contract, day, *values in rows:
                open_, high, low, close, settlement, volume, amount, interest = values
                months.setdefault(contract, Daily(contract, {}))
                months[contract].days[day.isoformat()] = dict(
                    auction=None not in (open_, high, low), open=open_, high=high, low=low,
                    close=close, settlement=settlement, volume=volume, amount=amount,
                    open_interest=interest)
            self.products[key] = months
        return self.products[key]

    def minute_bars(self, contract: str, begin: str, end: str, minutes: int) -> list:
        """A contract's bars of `minutes` from its stored minutes of the
        trading days in [begin, end] (asterion_lab/chart.py)."""
        rows = self.warehouse.query(self.version, """
            select day, start_ms, partial, open, high, low, close, volume, open_interest
            from minutes where contract = ? and day between ?::date and ?::date
            order by start_ms""", ("minutes",), (contract, begin, end),
            contract=contract, begin=begin, end=end)
        return chart.minute_bars(
            [(day.isoformat(), *values) for day, *values in rows], minutes)

    def contract_days(self, exchange: str, contract: str, begin: str, end: str) -> list:
        rows = self.warehouse.query(self.version, """
            select distinct list_date, delist_date from contracts where contract = ?""",
            ("contracts",), (contract,), exchange=exchange)
        if len(rows) != 1 or None in rows[0]:
            raise ValueError(f"the contract table has no unique listed life of {contract}")
        listed, delisted = rows[0]
        begin = max(datetime.date.fromisoformat(begin), listed)
        end = min(datetime.date.fromisoformat(end), delisted)
        rows = self.warehouse.query(self.version, """
            select day, open from calendar where exchange = ?
            and day between ?::date and ?::date order by day""", ("calendar",),
            (exchange, begin, end), exchange=exchange)
        days = [(begin + datetime.timedelta(days=n)).isoformat() for n in range((end - begin).days + 1)]
        calendar = {day.isoformat(): opened for day, opened in rows}
        missing = [day for day in days if calendar.get(day) is None]
        if missing:
            raise ValueError(f"the calendar of {exchange} is incomplete on {spans(missing, days)}")
        return [day for day in days if calendar[day]]

    def price_increments(self, exchange: str, contract: str, days: list) -> dict:
        rows = self.warehouse.query(self.version, """
            select effective_from, effective_until, price_tick from price_ticks
            where exchange = ? and (contract = ? or (contract is null and product = ?))""", ("price_ticks",),
            (exchange, contract, contract.split("/")[1]), exchange=exchange)
        values = {day: set() for day in days}
        for begin, end, tick in rows:
            if begin is None or end is None or begin >= end:
                raise ValueError(f"invalid price tick interval of {contract} on {begin or days[0]}")
            covered = days[bisect_left(days, begin.isoformat()):bisect_left(days, end.isoformat())]
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

    def costs(self, exchange: str, contract: str, days: list) -> list:
        """The contract's exchange margin and fees on each of `days` (ascending), from the
        version's settlement parameters, as the engine's dated costs. A fee the exchange
        published for no source stops the run instead of being replaced."""
        rows = self.warehouse.query(self.version, """
            select day, long_margin_rate, short_margin_rate, fee_rate, fee_per_lot,
                   close_today_fee_rate, close_today_fee_per_lot
            from settlement where contract = ?""", ("settlement",), (contract,),
            exchange=exchange, begin=days[0], end=days[-1])
        published = {day.isoformat(): values for day, *values in rows}
        schedule = []
        for day in days:
            values = published.get(day)
            if values is None:
                raise ValueError(f"no settlement parameters of {contract} on {day}")
            if None in values:
                raise ValueError(f"missing fees of {contract} on {day}")
            long, short, rate, per_lot, today_rate, today_per_lot = values
            if long != short:
                raise ValueError(f"long and short margin rates of {contract} differ on {day}")
            # As COSTS names them: margins are rates only; closing yesterday costs what opening does.
            costs = [raw(value) for value in (Decimal(0), per_lot, today_per_lot, per_lot,
                                              long, rate, today_rate, rate)]
            if not schedule or schedule[-1]["values"] != costs:
                schedule.append(dict(effective_from=day, source="warehouse", values=costs))
        return schedule

    def multiplier(self, exchange: str, contract: str) -> Decimal:
        if contract not in self.multipliers:
            self.multipliers[contract] = self.read_multiplier(exchange, contract)
        return self.multipliers[contract]

    def read_multiplier(self, exchange: str, contract: str) -> Decimal:
        product = contract.split("/")[1]
        if (exchange, product) not in self.units:
            units = {}
            for name, *row in self.warehouse.query(self.version, """
                    select distinct contract, multiplier, per_unit, trade_unit, quote_unit
                    from contracts where exchange = ? and product = ?""", ("contracts",),
                    (exchange, product), exchange=exchange):
                units.setdefault(name, []).append(tuple(row))
            self.units[exchange, product] = units
        rows = self.units[exchange, product].get(contract)
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
        implied = [Decimal(row["amount"]) / Decimal(row["volume"]) / Decimal(row["settlement"])
                   for day, row in self.product(exchange, product)[contract].days.items()
                   if self.begin <= day <= self.end and row["amount"] and row["volume"]
                   and row["settlement"]]
        agree = sum(abs(value / result - 1) <= Decimal("0.05") for value in implied)
        if agree * 2 < len(implied):
            median = sorted(implied)[len(implied) // 2]
            raise ValueError(f"the contract multiplier does not match the turnover of {contract}: "
                             f"its days imply about {float(median):g}")
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
    return ", ".join(run[0] if len(run) == 1 else f"{run[0]} to {run[-1]} ({len(run)} days)"
                     for run in runs)


def contract_input(data: Data, config: Config, contract: str, begin: str, end: str) -> dict:
    exchange, product, _ = contract.split("/")
    month = data.product(exchange, product).get(contract)
    if config.minutes:
        days = data.contract_days(exchange, contract, begin, end)
        missing = [day for day in days if month is None or day not in month.days]
        if missing:
            raise ValueError(f"daily bars of {contract} are not stored on {spans(missing, days)}")
        unsettled = [day for day in days if month.days[day]["settlement"] is None]
        if unsettled:
            raise ValueError(f"settlement prices of {contract} are missing on {spans(unsettled, days)}")
    else:
        if month is None:
            raise ValueError(f"no daily bars of {contract}")
        days = month.bars(begin, end)
    if not days:
        raise ValueError(f"no bars of {contract} fall inside the requested trading days")
    interest = lambda value: math.nan if value is None else float(value)
    excluded = []
    if config.minutes:
        # Minutes of the days that settle; a period is labelled when it ends.
        settled = set(days)
        minutes = [bar for bar in data.minute_bars(contract, begin, end, config.minutes)
                   if bar["trading_day"] in settled]
        stored = {bar["trading_day"] for bar in minutes}
        missing = [day for day in days if day not in stored and month.days[day]["volume"] != 0]
        if missing:
            raise ValueError(f"minutes of {contract} are not stored on {spans(missing, days)}")
        excluded = [bar["trading_day"] for bar in minutes if bar["partial"]]
        bars = [(bar["trading_day"], bar["time_ms"] * 10 ** 6,
                 *(raw(bar[name]) for name in ("open", "high", "low", "close", "volume")),
                 interest(bar["open_interest"]))
                for bar in minutes if not bar["partial"]]
        if not bars:
            raise ValueError(f"no stored minutes of {contract} fall inside the requested trading days")
    else:
        bars = [(day, close_ns(day), *(raw(month.days[day][name]) for name in
                                       ("open", "high", "low", "close", "volume")),
                 interest(month.days[day]["open_interest"])) for day in days]
    settlements = [(day, raw(row["settlement"])) for day, row in sorted(month.days.items())
                   if begin <= day <= end and row["settlement"] is not None]
    tick_days = sorted({bar[0] for bar in bars}
                       | {day for day, _ in settlements if day >= bars[0][0]})
    increments = data.price_increments(exchange, contract, tick_days)
    changes, current = [], increments[bars[0][0]]
    for day in tick_days[1:]:
        if increments[day] != current:
            current = increments[day]
            changes.append((day, current))
    if config.costs is None:
        costs = data.costs(exchange, contract, tick_days)
        # The account prices every contract from the run's first day on; before its first
        # bar a contract can neither fill nor be held, so its first fees are never charged early.
        costs[0]["effective_from"] = config.begin_day
    else:
        # A replay that trades nothing prices every contract from its first day on.
        costs = [dict(effective_from=config.begin_day, source="replay",
                      values=[raw(value) for value in config.costs])]
    return dict(
        venue=exchange, symbol=contract, price_increment=increments[bars[0][0]],
        quantity_increment=R, multiplier=raw(data.multiplier(exchange, contract)),
        costs=costs,
        increments=changes, slippage_ticks=config.slippage_ticks,
        bars=[bar[:7] for bar in bars],
        settlements=settlements,
        # Not the engine's: what a strategy reads besides prices, by bar time,
        # and the trading day of each bar left out.
        open_interest={bar[1]: bar[7] for bar in bars}, excluded=excluded)


def series_input(data: Data, config: Config, exchange: str, product: str):
    """The months of a product's traded dominant series and its rolls."""
    traded = {}
    for contract, month in data.product(exchange, product).items():
        bars = month.bars(config.begin_day, config.end_day)
        if bars:
            traded[contract] = bars
    if len(traded) < 2:
        raise ValueError("a dominant series requires at least two month contracts")
    months = []
    for contract in sorted(traded):
        item = dominant.Month(contract, contract.rsplit("/", 1)[1], last_bar=traded[contract][-1])
        for day, row in data.product(exchange, product)[contract].days.items():
            if config.begin_day <= day <= config.end_day:
                if row["settlement"] is not None:
                    item.settlement[day] = Decimal(row["settlement"])
                if row["open_interest"] is not None:
                    item.open_interest[day] = Decimal(row["open_interest"])
        months.append(item)
    plan = dominant.plan(months, traded=True)
    contracts, rolls = [], []
    for i, segment in enumerate(plan.segments):
        month = months[segment.month]
        # A paused strategy can keep the old month until it gives a new target.
        begin = segment.before if i else segment.begin
        contracts.append(contract_input(data, config, month.contract, begin, config.end_day))
        contracts[-1]["roll_ratio"] = float(plan.ratios[i])
        rolls.append((segment.begin, i))
    return contracts, rolls


def prepare(data: Data, config: Config):
    """The engine's contracts and series, and for each symbol the index of its
    first contract in them. Each contract is marked with the symbol it trades for."""
    contracts, series, first = [], [], {}
    for symbol in config.symbols:
        parts = symbol.split("/")
        first[symbol] = start = len(contracts)
        if len(parts) == 3:
            contracts.append(contract_input(data, config, symbol, config.begin_day, config.end_day))
        elif len(parts) == 2:
            months, rolls = series_input(data, config, *parts)
            offset = len(contracts)
            contracts.extend(months)
            series.append([(day, offset + index) for day, index in rolls])
        else:
            raise ValueError(f"a symbol is EXCHANGE/product/YYYY-MM or EXCHANGE/product: {symbol}")
        for item in contracts[start:]:
            item["run_symbol"] = symbol
    return contracts, series, first


def rolls(contracts: list) -> dict:
    """Each month a series rolls into, with its roll's ratio, for a Context."""
    return {item["symbol"]: item["roll_ratio"] for item in contracts if "roll_ratio" in item}


def visible(replayed, symbols: dict, contracts: list) -> list:
    """What a strategy reads of one timestamp's bars: those of the months its
    symbols trade now."""
    bars = []
    for _, contract, unit, leading, day, time_ns, *prices in replayed:
        if leading:
            open_, high, low, close, volume = (value / R for value in prices)
            item = contracts[contract]
            bars.append(Bar(symbols[unit], item["symbol"],
                            datetime.datetime.fromtimestamp(time_ns / 10 ** 9, BEIJING),
                            open_, high, low, close, volume, item["open_interest"][time_ns]))
    return bars


def observations(engine, symbols: dict, contracts: list):
    """Replay the same visible bars for research and strategy warmup, without
    submitting targets. The caller decides whether a bar should trade."""
    while not engine.finished:
        while not engine.day_done:
            yield visible(engine.next(), symbols, contracts)
        engine.settle()


def warmup(engine, strategy, symbols: dict, contracts: list) -> Context:
    """Rebuild strategy state from history. No target reaches the engine;
    the returned context continues with the next completed live bar."""
    context = Context([symbols[unit] for unit in sorted(symbols)], rolls(contracts))
    for bars in observations(engine, symbols, contracts):
        if bars:
            step(strategy, context, bars)
    # Historical intent must not be mistaken for a new live decision.
    context.wanted = {}
    return context


def replay(engine, strategy, symbols: dict, contracts: list, config: Config,
           decided=lambda bars, wanted: None) -> Context:
    """Drives the strategy through the engine. `symbols` maps each unit to its
    symbol; `decided` sees each time's bars and what the strategy asked for."""
    context = Context([symbols[unit] for unit in sorted(symbols)], rolls(contracts))
    units = {symbol: unit for unit, symbol in symbols.items()}
    for bars in observations(engine, symbols, contracts):
        if not bars:
            continue
        wanted = step(strategy, context, bars)
        decided(bars, wanted)
        if not context.ready:
            for unit in symbols:
                engine.want(unit, None)
            continue
        if context.mode == "weights" and wanted:
            # Weights are sized once, when given, at the closes of that time.
            wanted = engine.weights(
                {units[symbol]: raw(Decimal(weight).quantize(PLACES))
                 for symbol, weight in wanted.items()},
                raw(config.max_gross_weight), raw(config.max_net_weight))
        else:
            wanted = {units[symbol]: lots * R for symbol, lots in wanted.items()}
        for unit, quantity in wanted.items():
            engine.want(unit, quantity)
        # Lots stand: each bar places what is left of them again at its close.
        engine.decide()
    return context


def amount(values) -> pl.Series:
    return pl.Series([Decimal(value).scaleb(-8) for value in values], dtype=AMOUNT)


def tables(engine, contracts: list) -> dict:
    names = [item["symbol"] for item in contracts]
    owners = [item["run_symbol"] for item in contracts]
    orders = engine.orders()
    by_order = {order[0]: order for order in orders}
    fills = engine.fills()
    return {
        "equity": pl.DataFrame({"time_ns": [point[0] for point in engine.equity],
                                "equity": amount(point[1] for point in engine.equity),
                                "event": [point[2] for point in engine.equity]},
                               schema=TABLES["equity"]),
        "orders": pl.DataFrame({
            "order": [order[0] for order in orders], "time_ns": [order[8] for order in orders],
            "trading_day": [datetime.date.fromisoformat(order[9]) for order in orders],
            "symbol": [owners[order[1]] for order in orders],
            "contract": [names[order[1]] for order in orders],
            "side": [order[2] for order in orders], "offset": [order[3] for order in orders],
            "quantity": amount(order[4] for order in orders),
            "price": amount(order[5] for order in orders),
            "state": [order[6] for order in orders]}, schema=TABLES["orders"]),
        "fills": pl.DataFrame({
            "fill": [fill[0] for fill in fills], "order": [fill[1] for fill in fills],
            "time_ns": [fill[5] for fill in fills],
            "trading_day": [datetime.date.fromisoformat(fill[6]) for fill in fills],
            "contract": [names[by_order[fill[1]][1]] for fill in fills],
            "side": [by_order[fill[1]][2] for fill in fills],
            "offset": [by_order[fill[1]][3] for fill in fills],
            "quantity": amount(fill[2] for fill in fills), "price": amount(fill[3] for fill in fills),
            "fee": amount(fill[4] for fill in fills)}, schema=TABLES["fills"]),
        "positions": pl.DataFrame({
            "trading_day": [datetime.date.fromisoformat(row[0]) for row in engine.positions],
            "contract": [names[row[1]] for row in engine.positions],
            **{name: amount(row[i] for row in engine.positions)
               for i, name in ((2, "long"), (3, "short"), (4, "settlement_price"), (5, "margin"))}},
            schema=TABLES["positions"]),
        "settlements": pl.DataFrame({
            "trading_day": [datetime.date.fromisoformat(row[0]) for row in engine.settlements],
            "time_ns": [row[1] for row in engine.settlements],
            **{name: amount(row[i] for row in engine.settlements)
               for i, name in ((2, "balance"), (3, "equity"), (4, "realized"), (5, "fees"),
                               (6, "margin"), (7, "available"))}}, schema=TABLES["settlements"]),
    }


STATISTICS = ("total_return", "max_drawdown", "winning_days", "annual_return",
              "annual_volatility", "sharpe", "calmar")


def segment(config: Config, engine, first: int, last: int, base: int, begin_ns: int,
            opening: bool) -> dict:
    """The statistics of settled days [first, last) from `base` equity."""
    import asterion_engine

    marks, days = engine.equity, engine.settlements
    # Where each settlement is among the equity observations.
    ends = [n for n, point in enumerate(marks) if point[2] == "settlement"]
    values = asterion_engine.performance(
        base, [(row[0], row[3]) for row in days[first:last]],
        [point[1] for point in marks[(ends[first - 1] + 1 if first else 0):ends[last - 1] + 1]],
        begin_ns, days[last - 1][1], config.annual_periods, config.zero_risk_free, opening)
    return {name: values[name] for name in STATISTICS}


def segment_metrics(config: Config, engine, split: int) -> dict:
    """The statistics of the days from index `split` on, from the equity before them."""
    days = engine.settlements
    return segment(config, engine, split, len(days), days[split - 1][3], days[split - 1][1], True)


def metrics(config: Config, engine) -> dict:
    """The old engine's statistics of the whole run, and with a holdout day of
    the days before it and of the holdout alone."""
    days = engine.settlements
    if not days:
        raise ValueError("the run settled no trading day")
    result = segment(config, engine, 0, len(days), raw(config.deposit), engine.equity[0][0], False)
    if config.holdout_day:
        split = next((n for n, row in enumerate(days) if row[0] >= config.holdout_day), len(days))
        if not 0 < split < len(days):
            raise ValueError("the holdout day leaves no days before it or after it")
        for name, value in segment(config, engine, 0, split, raw(config.deposit),
                                   engine.equity[0][0], False).items():
            result[f"development_{name}"] = value
        for name, value in segment_metrics(config, engine, split).items():
            result[f"holdout_{name}"] = value
    return result


def costs_record(config: Config) -> dict:
    return {"fees": "exchange settlement parameters of each trading day",
            "slippage_ticks": config.slippage_ticks, "deposit": str(config.deposit)}


def risk_record(config: Config) -> dict:
    return {"max_gross_weight": str(config.max_gross_weight), "max_net_weight": str(config.max_net_weight),
            "max_order_lots": str(config.max_order_lots), "max_gross_lots": str(config.max_gross_lots),
            "max_working_orders": config.max_working_orders}


def statistics_record(config: Config) -> dict:
    return {"annual_periods": config.annual_periods, "zero_risk_free": config.zero_risk_free,
            "holdout_day": config.holdout_day}


def open_data(directory: Path, config: Config) -> Data:
    """The admitted version of the research dataset."""
    if not config.dataset_version:
        raise ValueError("research inputs must name the admitted dataset version")
    return Data(Dataset(directory), config.begin_day, config.end_day, version=config.dataset_version)


def excluded_record(contracts: list) -> dict:
    """The bars left out for an incomplete minute, and their trading days."""
    days = [day for item in contracts for day in item["excluded"]]
    return {"bars": len(days), "days": sorted(set(days))}


def bars_record(config: Config) -> dict:
    return {"period": f"{config.minutes}m" if config.minutes else "day"}


def plan(config: Config, version: str) -> Plan:
    return Plan(
        repository=config.repository, strategy=config.strategy,
        revision=repository.Revision(config.commit, config.changes),
        params=config.params, dataset_version=version,
        symbols=tuple(config.symbols), begin_day=config.begin_day, end_day=config.end_day,
        costs=costs_record(config), risk=risk_record(config), statistics=statistics_record(config),
        bars=bars_record(config))


def run(directory: Path, identity: str, config: Config, started=lambda: None):
    """One backtest from start to its record. A failure is recorded as failed;
    `started` is called once the record exists."""
    import asterion_engine

    runs = Runs(directory)
    if not config.source:
        raise ValueError("research runs read the source copied at admission")
    data = open_data(directory, config)
    runs.start(identity, plan(config, data.version.identity))
    started()
    try:
        strategy = repository.load(Path(config.source), config.strategy, config.params)
        contracts, series, first = prepare(data, config)
        engine = asterion_engine.Replay(
            raw(config.deposit), contracts, series,
            (raw(config.max_order_lots), raw(config.max_gross_lots), config.max_working_orders))
        symbols = {engine.unit(index): symbol for symbol, index in first.items()}
        replay(engine, strategy, symbols, contracts, config)
        runs.succeed(identity, tables(engine, contracts), metrics(config, engine),
                     {"excluded": excluded_record(contracts)} if config.minutes else {})
    except BaseException as error:
        runs.end(identity, "failed", str(error) or type(error).__name__)
        raise


def number(name: str, text: str) -> Decimal:
    """A decimal setting of a run, named when it is missing or malformed."""
    try:
        value = Decimal(text)
    except ArithmeticError:
        value = None
    if value is None or not value.is_finite():
        raise ValueError(f"{name} must be a number, not {text!r}")
    return value


def minutes_of(period) -> int:
    """Minutes of a bar of `period`; 0 for daily bars."""
    from asterion.common_pb2 import Period
    minutes = {Period.PERIOD_DAY: 0, Period.PERIOD_MINUTES_1: 1, Period.PERIOD_MINUTES_5: 5,
               Period.PERIOD_MINUTES_10: 10, Period.PERIOD_MINUTES_15: 15,
               Period.PERIOD_MINUTES_30: 30, Period.PERIOD_MINUTES_60: 60}
    if period not in minutes:
        raise ValueError(f"backtest bars are daily or {', '.join(map(str, chart.MINUTES))} minutes")
    return minutes[period]


def config_of(admitted) -> Config:
    """A Config from an AdmittedRun message."""
    request = admitted.backtest
    params = {name: getattr(value, value.WhichOneof("value"))
              for name, value in request.params.items() if value.WhichOneof("value")}
    limits = request.limits
    return Config(
        repository=request.repository, strategy=request.strategy, params=params,
        symbols=tuple(request.symbols),
        begin_day=request.first_day, end_day=request.last_day,
        deposit=number("deposit", request.deposit.text),
        costs=None, slippage_ticks=request.slippage_ticks,
        max_order_lots=number("max_order_lots", limits.max_order_lots.text),
        max_gross_lots=number("max_gross_lots", limits.max_gross_lots.text),
        max_working_orders=limits.max_working_orders,
        max_gross_weight=number("max_gross_weight", request.max_gross_weight.text or "0"),
        max_net_weight=number("max_net_weight", request.max_net_weight.text or "0"),
        annual_periods=request.periods_per_year or None,
        zero_risk_free=request.zero_risk_free_rate,
        holdout_day=request.holdout_day, minutes=minutes_of(request.period),
        dataset_version=admitted.pinned.dataset_version, source=admitted.source,
        commit=admitted.pinned.commit, changes=admitted.pinned.changes)


def watch_parent():
    """Ends this process when the service that started it is gone; the next
    service marks the run interrupted."""
    import os
    import threading
    import time

    parent = os.getppid()

    def watch():
        while os.getppid() == parent:
            time.sleep(1)
        os._exit(1)

    threading.Thread(target=watch, daemon=True).start()


def main():
    from asterion.lab_pb2 import AdmittedRun

    watch_parent()
    request = AdmittedRun()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request.backtest.id, config_of(request),
            lambda: print("started", flush=True))
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.exit(1)
