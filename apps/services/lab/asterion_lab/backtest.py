"""Backtests: a strategy of the user's repository over the warehouse's daily
bars, through asterion_engine (core's Replay).

The data are read as the old Data service reads them, so that both engines
see the same bars:

- A contract has a bar on a trading day where it traded in the auction (open,
  high and low present) and settled; the bar is labelled 15:00 Shanghai.
- Its multiplier is the contract table's, times the lot in the unit the
  price is quoted per; most days with turnover must agree with it within 5%.
- A day's price increment is the greatest common divisor of the product's
  auction prices over 20 traded days: the larger of the 20 up to the day and
  the 20 from it, on the stored days around the range.
- A product's dominant series (symbol EXCHANGE/product) trades its dominant
  months by the rule of dominant.py; a later month joins a day before its
  roll, and a month that hands over stays until its bar on or after the roll
  day and the next one.

Costs, slippage, order limits and the weight budgets are the run's, as the
user gives them. Lots a strategy targets stand and are placed again at every
bar's close; weights are sized into lots once, when given.

The service runs each backtest in a child process:

    python3 -I -B -c "...; from asterion_lab.backtest import main; main()" <lab-directory>

reads its BacktestRun (asterion/lab.proto) from standard input.
"""
from dataclasses import dataclass
import datetime
from decimal import Decimal
import math
import sys
from pathlib import Path

import polars as pl

from asterion_lab import dominant, repository
from asterion_lab.runs import AMOUNT, TABLES, Plan, Runs
from asterion_lab.strategy import Bar, Context, step
from asterion_lab.warehouse import Warehouse

R = 10 ** 8
WINDOW = 20
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
    provider: str
    symbols: tuple
    begin_day: str
    end_day: str
    deposit: Decimal
    costs: dict  # EXCHANGE/product -> eight decimals, as COSTS names them
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
    """The warehouse's daily table of a version, read per product."""

    def __init__(self, warehouse: Warehouse, begin: str, end: str):
        self.warehouse = warehouse
        self.version = warehouse.current()
        self.begin, self.end = begin, end
        # Read once per run: each product's months, increments and contract units.
        self.products, self.increment_days, self.units, self.multipliers = {}, {}, {}, {}

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

    def increments(self, exchange: str, product: str) -> dict:
        """The price increment of each traded day in the range."""
        key = (exchange, product)
        if key not in self.increment_days:
            self.increment_days[key] = self.read_increments(exchange, product)
        return self.increment_days[key]

    def read_increments(self, exchange: str, product: str) -> dict:
        stored = sorted(day for day in self.version.days(exchange))
        first = next((i for i, day in enumerate(stored) if day >= self.begin), len(stored))
        last = next((i for i in range(len(stored) - 1, -1, -1) if stored[i] <= self.end), -1)
        read = set(stored[max(0, first - WINDOW):last + 1 + WINDOW])
        divisors = {}
        for month in self.product(exchange, product).values():
            for day, row in month.days.items():
                if day not in read or not row["auction"] or row["close"] is None:
                    continue
                divisor = 0
                for name in ("open", "high", "low", "close"):
                    divisor = math.gcd(divisor, raw(row[name]))
                divisors[day] = math.gcd(divisors.get(day, 0), divisor)
        traded = sorted(divisors.items())
        window = min(WINDOW, len(traded))

        def divisor(start):
            result = 0
            for _, value in traded[start:start + window]:
                result = math.gcd(result, value)
            return result

        return {day: max(divisor(i + 1 - window if i + 1 >= window else 0),
                         divisor(min(i, len(traded) - window)))
                for i, (day, _) in enumerate(traded) if self.begin <= day <= self.end}

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


def contract_input(data: Data, config: Config, contract: str, begin: str, end: str) -> dict:
    exchange, product, _ = contract.split("/")
    month = data.product(exchange, product).get(contract)
    if month is None:
        raise ValueError(f"no daily bars of {contract}")
    days = month.bars(begin, end)
    if not days:
        raise ValueError(f"no bars of {contract} fall inside the requested trading days")
    increments = data.increments(exchange, product)
    changes, current = [], increments[days[0]]
    for day in days[1:]:
        if increments[day] != current:
            current = increments[day]
            changes.append((day, current))
    costs = config.costs.get(f"{exchange}/{product}")
    if costs is None:
        raise ValueError(f"the run gives no costs of {exchange}/{product}")
    return dict(
        venue=exchange, symbol=contract, price_increment=increments[days[0]],
        quantity_increment=R, multiplier=raw(data.multiplier(exchange, contract)),
        # The account prices every contract from the run's first day on.
        costs=[dict(effective_from=config.begin_day, source="run",
                    values=[raw(value) for value in costs])],
        increments=changes, slippage_ticks=config.slippage_ticks,
        bars=[(day, close_ns(day), *(raw(month.days[day][name]) for name in
                                     ("open", "high", "low", "close", "volume"))) for day in days],
        settlements=[(day, raw(row["settlement"])) for day, row in sorted(month.days.items())
                     if begin <= day <= end and row["settlement"] is not None],
        # Not the engine's: what a strategy reads besides prices.
        open_interest={day: math.nan if month.days[day]["open_interest"] is None
                       else float(month.days[day]["open_interest"]) for day in days})


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
        # A later month joins a day early; one that hands over stays for its
        # bar on or after the roll day, and the next.
        begin = segment.before if i else segment.begin
        if i + 1 < len(plan.segments):
            bars = traded[month.contract]
            closed = next(n for n, day in enumerate(bars) if day >= plan.segments[i + 1].begin)
            end = bars[min(closed + 1, len(bars) - 1)]
        else:
            end = plan.days[-1]
        contracts.append(contract_input(data, config, month.contract, begin, end))
        rolls.append((segment.begin, i, raw(segment.factor)))
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
            series.append([(day, offset + index, factor) for day, index, factor in rolls])
        else:
            raise ValueError(f"a symbol is EXCHANGE/product/YYYY-MM or EXCHANGE/product: {symbol}")
        for item in contracts[start:]:
            item["run_symbol"] = symbol
    return contracts, series, first


def visible(replayed, symbols: dict, contracts: list) -> list:
    """What a strategy reads of one timestamp's bars: those of the months its
    symbols trade now."""
    bars = []
    for _, contract, unit, leading, day, time_ns, *prices, _ in replayed:
        if leading:
            open_, high, low, close, volume = (value / R for value in prices)
            item = contracts[contract]
            bars.append(Bar(symbols[unit], item["symbol"],
                            datetime.datetime.fromtimestamp(time_ns / 10 ** 9, BEIJING),
                            open_, high, low, close, volume, item["open_interest"][day]))
    return bars


def replay(engine, strategy, symbols: dict, contracts: list, config: Config):
    """Drives the strategy through the engine. `symbols` maps each unit to its symbol."""
    context = Context([symbols[unit] for unit in sorted(symbols)])
    units = {symbol: unit for unit, symbol in symbols.items()}
    while not engine.finished:
        while not engine.day_done:
            bars = visible(engine.next(), symbols, contracts)
            wanted = step(strategy, context, bars) if bars else {}
            if context.mode != "weights":
                # Lots stand: each bar places them again at its close.
                for symbol, lots in wanted.items():
                    engine.want(units[symbol], lots * R)
                engine.decide()
            elif wanted:
                # Weights are sized once, when given, at the closes of that time.
                lots = engine.weights(
                    {units[symbol]: raw(Decimal(weight).quantize(PLACES))
                     for symbol, weight in wanted.items()},
                    raw(config.max_gross_weight), raw(config.max_net_weight))
                for unit, quantity in lots.items():
                    engine.want(unit, quantity)
                engine.decide()
        engine.settle()


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
            "symbol": [owners[order[1]] for order in orders],
            "contract": [names[order[1]] for order in orders],
            "side": [order[2] for order in orders], "offset": [order[3] for order in orders],
            "quantity": amount(order[4] for order in orders),
            "price": amount(order[5] for order in orders),
            "state": [order[6] for order in orders]}, schema=TABLES["orders"]),
        "fills": pl.DataFrame({
            "fill": [fill[0] for fill in fills], "order": [fill[1] for fill in fills],
            "time_ns": [fill[5] for fill in fills],
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
    return {"by_product": {key: dict(zip(COSTS, map(str, values))) for key, values in config.costs.items()},
            "slippage_ticks": config.slippage_ticks, "deposit": str(config.deposit)}


def risk_record(config: Config) -> dict:
    return {"max_gross_weight": str(config.max_gross_weight), "max_net_weight": str(config.max_net_weight),
            "max_order_lots": str(config.max_order_lots), "max_gross_lots": str(config.max_gross_lots),
            "max_working_orders": config.max_working_orders}


def statistics_record(config: Config) -> dict:
    return {"annual_periods": config.annual_periods, "zero_risk_free": config.zero_risk_free,
            "holdout_day": config.holdout_day}


def run(directory: Path, identity: str, config: Config, started=lambda: None):
    """One backtest from start to its record. A failure is recorded as failed;
    `started` is called once the record exists."""
    import asterion_engine

    runs = Runs(directory)
    root = Path(config.repository)
    revision = repository.revision(repository.checked(root))
    warehouse = Warehouse(directory) if config.provider == "tushare" else \
        Warehouse(directory / "sources" / config.provider, config.provider)
    data = Data(warehouse, config.begin_day, config.end_day)
    runs.start(identity, Plan(
        repository=config.repository, strategy=config.strategy, revision=revision,
        params=config.params, provider=config.provider, dataset_version=data.version.identity,
        symbols=tuple(config.symbols), begin_day=config.begin_day, end_day=config.end_day,
        costs=costs_record(config), risk=risk_record(config), statistics=statistics_record(config)))
    started()
    try:
        strategy = repository.load(root, config.strategy, config.params)
        contracts, series, first = prepare(data, config)
        engine = asterion_engine.Replay(
            raw(config.deposit), contracts, series,
            (raw(config.max_order_lots), raw(config.max_gross_lots), config.max_working_orders))
        symbols = {engine.unit(index): symbol for symbol, index in first.items()}
        replay(engine, strategy, symbols, contracts, config)
        runs.succeed(identity, tables(engine, contracts), metrics(config, engine))
    except BaseException as error:
        runs.fail(identity, "failed", str(error) or type(error).__name__)
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


def config_of(request) -> Config:
    """A Config from a BacktestRun message."""
    params = {name: getattr(value, value.WhichOneof("value"))
              for name, value in request.params.items() if value.WhichOneof("value")}
    costs = {}
    for item in request.costs:
        if len(item.values) != len(COSTS):
            raise ValueError(f"costs of {item.product} have eight values: {', '.join(COSTS)}")
        costs[item.product] = [number(f"{item.product} {name}", value)
                               for name, value in zip(COSTS, item.values)]
    return Config(
        repository=request.repository, strategy=request.strategy, params=params,
        provider=request.provider or "tushare", symbols=tuple(request.symbols),
        begin_day=request.begin_day, end_day=request.end_day, deposit=number("deposit", request.deposit),
        costs=costs, slippage_ticks=request.slippage_ticks,
        max_order_lots=number("max_order_lots", request.max_order_lots),
        max_gross_lots=number("max_gross_lots", request.max_gross_lots),
        max_working_orders=request.max_working_orders,
        max_gross_weight=number("max_gross_weight", request.max_gross_weight or "0"),
        max_net_weight=number("max_net_weight", request.max_net_weight or "0"),
        annual_periods=request.annual_periods or None, zero_risk_free=request.zero_risk_free,
        holdout_day=request.holdout_day)


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
    from asterion.lab_pb2 import BacktestRun

    watch_parent()
    request = BacktestRun()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request.id, config_of(request),
            lambda: print("started", flush=True))
    except BaseException as error:
        print(error, file=sys.stderr)
        sys.exit(1)
