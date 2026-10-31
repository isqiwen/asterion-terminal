"""A factor evaluated across symbols, in a child process of the lab service.

A factor is a plain function of the strategy repository (see repository.py):

    def momentum(close, lookback: int = 20):
        \"\"\"Return over the last `lookback` bars.\"\"\"
        result = np.full(len(close), np.nan)
        result[lookback:] = close[lookback:] / close[:-lookback] - 1
        return result

It is called once per symbol with read-only arrays of that symbol's bars, in
order: exactly what a strategy's ctx.history returns, a dominant series at the
level of its latest month. It returns one value per bar, NaN where it has
none; the value of a bar may use that bar and those before it, never later
ones, which is also when a strategy could act on it.

Each day, the symbols with a value are ranked against their returns from that
close to the close `horizon` bars later (Spearman's rank correlation, the IC),
and split into `groups` equal groups by value, whose next-bar returns are
averaged. The child reads a FactorRun on standard input and writes a
FactorResult to standard output:

    python3 -I -B -c "...; from asterion_lab.factor import main; main()" <lab-directory>
"""
from decimal import Decimal
import math
import os
from pathlib import Path
import sys

import numpy as np
import polars as pl

from asterion_lab import repository
from asterion_lab.backtest import COSTS, Config, Data, prepare, raw, visible
from asterion_lab.strategy import FIELDS, KINDS
from asterion_lab.warehouse import Warehouse

MINIMUM = 3  # symbols a day's IC needs


def symbols_of(data: Data, symbols) -> tuple:
    """The symbols to evaluate: an exchange stands for each of its products
    with daily bars in the range. Returns them and the products left out,
    each with why: those that cannot form a series."""
    chosen, skipped = [], []
    for symbol in symbols:
        if "/" in symbol:
            chosen.append(symbol)
            continue
        data.check_stored(symbol)
        products = data.warehouse.query(data.version, """
            select distinct product from daily
            where exchange = ? and day between ?::date and ?::date order by product""",
            ("daily",), (symbol, data.begin, data.end), exchange=symbol)
        for (product,) in products:
            name = f"{symbol}/{product}"
            try:
                prepare(data, config(data, (name,)))
                chosen.append(name)
            except ValueError as error:
                skipped.append((name, str(error)))
    if len(set(chosen)) != len(chosen):
        raise ValueError("a symbol is given twice")
    return tuple(chosen), skipped


def config(data: Data, symbols) -> Config:
    """A replay that trades nothing: costs and limits play no part, but the
    account accepts only valid ones."""
    products = {"/".join(symbol.split("/")[:2]) for symbol in symbols}
    costs = [Decimal(1) if name == "margin_per_lot" else Decimal(0) for name in COSTS]
    return Config(repository="", strategy="", params={}, provider="", symbols=tuple(symbols),
                  begin_day=data.begin, end_day=data.end, deposit=Decimal(1),
                  costs=dict.fromkeys(products, costs),
                  slippage_ticks=0, max_order_lots=Decimal(0), max_gross_lots=Decimal(0),
                  max_working_orders=0)


def bars(data: Data, symbol: str) -> dict:
    """A symbol's bars as a strategy sees them: its days and an array of each
    field. Each symbol is replayed alone, so a contract and the series it is a
    month of can both be evaluated."""
    import asterion_engine

    contracts, series, _ = prepare(data, config(data, (symbol,)))
    limit = raw(Decimal(1))
    engine = asterion_engine.Replay(limit, contracts, series, (limit, limit, 1))
    columns = {"day": [], **{field: [] for field in FIELDS}}
    while not engine.finished:
        while not engine.day_done:
            for bar in visible(engine.next(), {0: symbol}, contracts):
                columns["day"].append(bar.time.date().isoformat())
                for field in FIELDS:
                    columns[field].append(getattr(bar, field))
            engine.decide()
        engine.settle()
    arrays = {field: np.array(columns[field], dtype=float) for field in FIELDS}
    for array in arrays.values():
        array.flags.writeable = False
    return {"day": columns["day"], **arrays}


def values(function, inputs, settings, symbols: dict) -> pl.DataFrame:
    """The factor's value and the close of every symbol's bars."""
    frames = []
    for symbol, columns in symbols.items():
        count = len(columns["day"])
        try:
            computed = function(**{name: columns[name] for name in inputs}, **settings)
            result = np.asarray(computed, dtype=float)
        except Exception as error:
            raise ValueError(f"the factor failed on {symbol}: {type(error).__name__}: {error}")
        if result.shape != (count,):
            raise ValueError(f"the factor gave {result.shape} values for the {count} bars of {symbol}")
        frames.append(pl.DataFrame({
            "symbol": [symbol] * count, "day": columns["day"], "bar": np.arange(count),
            "close": columns["close"], "value": result}))
    return pl.concat(frames).with_columns(
        pl.when(pl.col("value").is_finite()).then(pl.col("value")).otherwise(None))


def evaluate(frame: pl.DataFrame, horizons: list, groups: int) -> dict:
    """IC at each horizon, the days' IC and group returns at the first, and
    each group's mean return and turnover."""
    frame = frame.sort("symbol", "bar").with_columns([
        (pl.col("close").shift(-h).over("symbol") / pl.col("close") - 1).alias(f"ahead{h}")
        for h in horizons])
    days = sorted(set(frame["day"]))
    summary, first_ic = [], {}
    for n, h in enumerate(horizons):
        ranked = frame.filter(pl.col("value").is_not_null() & pl.col(f"ahead{h}").is_finite())
        daily = (ranked.group_by("day")
                 .agg(pl.len().alias("count"),
                      pl.corr("value", f"ahead{h}", method="spearman").alias("ic"))
                 .filter((pl.col("count") >= MINIMUM) & pl.col("ic").is_finite())
                 .sort("day"))
        ic = daily["ic"].to_numpy()
        if n == 0:
            first_ic = dict(zip(daily["day"], ic))
        summary.append(statistics(h, ic))
    returns, members = group_returns(frame, horizons[0], groups)
    # Cumulative from the first day with groups; a day without them keeps the last.
    total, long_short, state, history = [1.0] * groups, 1.0, None, []
    for day in days:
        if day in returns:
            total = [value * (1 + r) for value, r in zip(total, returns[day])]
            long_short *= 1 + returns[day][-1] - returns[day][0]
            state = (total, long_short)
        history.append(state)
    counts = dict(frame.filter(pl.col("value").is_not_null()).group_by("day").len().iter_rows())
    return {
        "horizons": summary,
        "days": [{"day": day, "symbols": counts.get(day, 0), "ic": first_ic.get(day),
                  "groups": state[0] if state else [], "long_short": state[1] if state else None}
                 for day, state in zip(days, history)],
        "groups": [{"group": g + 1,
                    "mean_return": mean([day[g] for day in returns.values()]),
                    "turnover": turnover([day[g] for day in members.values()])}
                   for g in range(groups)],
    }


def group_returns(frame: pl.DataFrame, horizon: int, groups: int):
    """Each day with at least `groups` symbols: the mean return `horizon` bars
    ahead of each group, and its members. Equal values keep symbol order."""
    returns, members = {}, {}
    ranked = frame.filter(pl.col("value").is_not_null() & pl.col(f"ahead{horizon}").is_finite())
    for (day,), part in ranked.sort("day", "value", "symbol").group_by("day", maintain_order=True):
        size = part.height
        if size < groups:
            continue
        bucket = (np.arange(size) * groups) // size
        ahead = part[f"ahead{horizon}"].to_numpy()
        symbols = part["symbol"].to_list()
        returns[day] = [float(ahead[bucket == g].mean()) for g in range(groups)]
        members[day] = [{symbols[i] for i in np.flatnonzero(bucket == g)} for g in range(groups)]
    return returns, members


def statistics(horizon: int, ic) -> dict:
    count = len(ic)
    result = {"horizon": horizon, "days": count, "mean": mean(ic), "deviation": None,
              "ir": None, "t": None, "positive": float((ic > 0).mean()) if count else None}
    if count > 1:
        deviation = float(np.std(ic, ddof=1))
        result["deviation"] = deviation
        if deviation > 0:
            result["ir"] = result["mean"] / deviation
            result["t"] = result["ir"] * math.sqrt(count)
    return result


def mean(values) -> float | None:
    return float(np.mean(values)) if len(values) else None


def turnover(sets: list) -> float | None:
    """The mean share of a group's members that were not in it the day before."""
    changes = [len(today - before) / len(today) for before, today in zip(sets, sets[1:]) if today]
    return mean(changes)


def settings_of(request, defaults: dict) -> dict:
    """The request's settings over the factor's defaults, each of its kind."""
    unknown = sorted(set(request.params) - set(defaults))
    if unknown:
        raise ValueError(f"unknown parameters: {', '.join(unknown)}")
    settings = dict(defaults)
    for name, value in request.params.items():
        field = value.WhichOneof("value")
        given = getattr(value, field) if field else None
        kind = type(defaults[name])
        if kind is float and type(given) is int:
            given = float(given)
        if type(given) is not kind:
            raise ValueError(f"parameter {name} is a {KINDS[kind]}, not {given!r}")
        settings[name] = given
    return settings


def run(directory: Path, request, reply):
    if not request.horizons or any(h < 1 for h in request.horizons) \
            or len(set(request.horizons)) != len(request.horizons):
        raise ValueError("horizons are distinct whole numbers of bars, at least one")
    if request.groups < 2:
        raise ValueError("a factor is split into at least two groups")
    if not request.symbols:
        raise ValueError("a factor is evaluated over one or more symbols")
    root = repository.checked(Path(request.repository))
    revision = repository.revision(root)
    function, inputs, defaults = repository.factor(root, request.factor)
    settings = settings_of(request, defaults)
    provider = request.provider or "tushare"
    warehouse = Warehouse(directory) if provider == "tushare" else \
        Warehouse(directory / "sources" / provider, provider)
    data = Data(warehouse, request.begin_day, request.end_day)
    symbols, skipped = symbols_of(data, list(request.symbols))
    if not symbols:
        raise ValueError("no symbol has daily bars in the range")
    result = evaluate(values(function, inputs, settings,
                             {symbol: bars(data, symbol) for symbol in symbols}),
                      list(request.horizons), request.groups)
    reply.commit, reply.changes = revision.commit, revision.changes
    reply.dataset_version = data.version.identity
    reply.symbols.extend(symbols)
    for symbol, reason in skipped:
        reply.skipped.add(symbol=symbol, reason=reason)
    for item in result["horizons"]:
        entry = reply.horizons.add(horizon=item["horizon"], days=item["days"])
        for name in ("mean", "deviation", "ir", "t", "positive"):
            if item[name] is not None:
                setattr(entry, name, item[name])
    for item in result["days"]:
        entry = reply.days.add(trading_day=item["day"], symbols=item["symbols"],
                               groups=item["groups"])
        if item["ic"] is not None:
            entry.ic = item["ic"]
        if item["long_short"] is not None:
            entry.long_short = item["long_short"]
    for item in result["groups"]:
        entry = reply.groups.add(group=item["group"])
        for name in ("mean_return", "turnover"):
            if item[name] is not None:
                setattr(entry, name, item[name])


def main():
    from asterion.lab_pb2 import FactorResult, FactorRun

    from asterion_lab.backtest import watch_parent

    watch_parent()
    # What factor code prints goes to standard error; standard output carries the reply.
    output = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    request, reply = FactorRun(), FactorResult()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request, reply)
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
    output.write(reply.SerializeToString())
    output.flush()
