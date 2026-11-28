"""Backtests: a strategy of the user's repository over the warehouse's daily
or minute bars, through asterion_engine (core's Replay).

Data loading and contract/dominant-series input preparation live in data.py.
Margins and fees are the exchange's settlement parameters of each trading
day in the pinned dataset; a day without them stops the run. Slippage, order
limits and the weight budgets are the run's, as the user gives them. Weights
are sized into lots once, when given. Lots stand and what is left of them is
placed again at every bar's close, each order at most the order limit. A
strategy that is not ready places nothing and keeps what it holds; once ready
again, only the targets it then gives stand.

The service runs each backtest in a child process:

    python3 -I -B -c "...; from asterion_lab.backtest import main; main()" <lab-directory>

reads its AdmittedRun (asterion/lab.proto) from standard input.
"""
from dataclasses import dataclass
import datetime
from decimal import Decimal
import functools
import math
from pathlib import Path
import sys

import numpy as np
import polars as pl

from asterion_lab import chart, diagnostics, repository
from asterion_lab.data import BEIJING, R, open_data, prepare, raw, spans
from asterion_lab.runs import AMOUNT, TABLES, Plan, Runs
from asterion_strategy import Bar, CombinedStrategy, Context, quantize_weight, step

_EMPTY_DICT: dict = {}
_NAN4 = (math.nan, math.nan, math.nan, math.nan)


@functools.lru_cache(maxsize=8192)
def _bar_time(time_ns: int) -> datetime.datetime:
    return datetime.datetime.fromtimestamp(time_ns / 10**9, BEIJING)


def _pack_bar_columns(contracts: list):
    """Attach columnar NumPy bar arrays to each contract once so Replay avoids per-tuple unpacking."""
    for item in contracts:
        if "bars_columns" in item:
            continue
        raw_bars = item["bars"]
        n = len(raw_bars)
        if n == 0:
            empty = np.empty(0, dtype=np.int64)
            item["bars_columns"] = ([], empty, empty, empty, empty, empty, empty)
        else:
            days = [b[0] for b in raw_bars]
            arr = np.asarray([(b[1], b[2], b[3], b[4], b[5], b[6]) for b in raw_bars], dtype=np.int64)
            item["bars_columns"] = (
                days,
                np.ascontiguousarray(arr[:, 0]),
                np.ascontiguousarray(arr[:, 1]),
                np.ascontiguousarray(arr[:, 2]),
                np.ascontiguousarray(arr[:, 3]),
                np.ascontiguousarray(arr[:, 4]),
                np.ascontiguousarray(arr[:, 5]),
            )


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
    max_contract_lots: Decimal
    max_working_orders: int
    # What ctx.weights may ask for in all, gross and net, as shares of equity.
    max_gross_weight: Decimal = Decimal(0)
    max_net_weight: Decimal = Decimal(0)
    max_margin_ratio: Decimal = Decimal(0)
    min_rebalance_weight: Decimal = Decimal(0)
    # Multiplier applied to exchange commission fees (margins unchanged).
    fee_multiplier: Decimal = Decimal(1)
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


def rolls(contracts: list) -> dict:
    """Each month a series rolls into, with its roll's ratio, for a Context."""
    return {item["symbol"]: item["roll_ratio"] for item in contracts if "roll_ratio" in item}


def _contract_views(contracts: list) -> list:
    return [
        (
            item["symbol"],
            item["open_interest"],
            item.get("roll_yield") or _EMPTY_DICT,
            item.get("far_close") or _EMPTY_DICT,
            item.get("near_return") or _EMPTY_DICT,
            item.get("far_return") or _EMPTY_DICT,
            item.get("pit") or _EMPTY_DICT,
        )
        for item in contracts
    ]


def visible(replayed, symbols: dict, contracts: list) -> list:
    """What a strategy reads of one timestamp's bars: those of the months its
    symbols trade now."""
    bars = []
    cached_ns = None
    cached_dt = None
    for _, contract, unit, leading, _, time_ns, o, h, lo, c, v in replayed:
        if leading:
            if time_ns != cached_ns:
                cached_ns = time_ns
                cached_dt = _bar_time(time_ns)
            item = contracts[contract]
            pit_map = item.get("pit")
            rv, ml, ms, inv = pit_map.get(time_ns, _NAN4) if pit_map else _NAN4
            ry_map = item.get("roll_yield")
            fc_map = item.get("far_close")
            nr_map = item.get("near_return")
            fr_map = item.get("far_return")
            bars.append(
                Bar(
                    symbols[unit],
                    item["symbol"],
                    cached_dt,
                    o / R,
                    h / R,
                    lo / R,
                    c / R,
                    v / R,
                    item["open_interest"][time_ns],
                    ry_map.get(time_ns, math.nan) if ry_map else math.nan,
                    fc_map.get(time_ns, math.nan) if fc_map else math.nan,
                    nr_map.get(time_ns, math.nan) if nr_map else math.nan,
                    fr_map.get(time_ns, math.nan) if fr_map else math.nan,
                    rv,
                    ml,
                    ms,
                    inv,
                )
            )
    return bars


def observations(engine, symbols: dict, contracts: list):
    """Replay the same visible bars for research and strategy warmup, without
    submitting targets. The caller decides whether a bar should trade."""
    views = _contract_views(contracts)
    has_fast = hasattr(engine, "next_leading")
    nan = math.nan
    nan4 = _NAN4
    inv_r = 1.0 / R
    while not engine.finished:
        while not engine.day_done:
            if not has_fast:
                yield visible(engine.next(), symbols, contracts)
                continue
            replayed = engine.next_leading()
            if not replayed:
                yield []
                continue
            bars = []
            cached_ns = None
            cached_dt = None
            for contract, unit, time_ns, o, h, lo, c, v in replayed:
                if time_ns != cached_ns:
                    cached_ns = time_ns
                    cached_dt = _bar_time(time_ns)
                sym, oi_map, ry_map, fc_map, nr_map, fr_map, pit_map = views[contract]
                rv, ml, ms, inv = pit_map.get(time_ns, nan4) if pit_map else nan4
                bars.append(
                    Bar(
                        symbols[unit],
                        sym,
                        cached_dt,
                        o * inv_r,
                        h * inv_r,
                        lo * inv_r,
                        c * inv_r,
                        v * inv_r,
                        oi_map[time_ns],
                        ry_map.get(time_ns, nan) if ry_map else nan,
                        fc_map.get(time_ns, nan) if fc_map else nan,
                        nr_map.get(time_ns, nan) if nr_map else nan,
                        fr_map.get(time_ns, nan) if fr_map else nan,
                        rv,
                        ml,
                        ms,
                        inv,
                    )
                )
            yield bars
        engine.settle()


def warmup(engine, strategy, symbols: dict, contracts: list) -> Context:
    """Rebuild strategy state from history. No target reaches the engine;
    the returned context continues with the next completed live bar."""
    context = Context(
        [symbols[unit] for unit in sorted(symbols)],
        rolls(contracts),
        max_history=getattr(strategy, "history_limit", None),
    )
    for bars in observations(engine, symbols, contracts):
        if bars:
            step(strategy, context, bars)
    # Historical intent must not be mistaken for a new live decision.
    context.wanted = {}
    return context


def replay(
    engine,
    strategy,
    symbols: dict,
    contracts: list,
    config: Config,
    decided=lambda bars, wanted: None,
) -> Context:
    """Drives the strategy through the engine. `symbols` maps each unit to its
    symbol; `decided` sees each time's bars and what the strategy asked for."""
    context = Context(
        [symbols[unit] for unit in sorted(symbols)],
        rolls(contracts),
        max_history=getattr(strategy, "history_limit", None),
    )
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
                {units[symbol]: raw(quantize_weight(weight)) for symbol, weight in wanted.items()},
                raw(config.max_gross_weight),
                raw(config.max_net_weight),
                raw(config.max_margin_ratio),
                raw(config.min_rebalance_weight),
            )
        else:
            wanted = {units[symbol]: lots * R for symbol, lots in wanted.items()}
        for unit, quantity in wanted.items():
            engine.want(unit, quantity)
        # Lots stand: each bar places what is left of them again at its close.
        engine.decide()
    return context


def amount(values) -> pl.Series:
    return pl.Series([Decimal(value).scaleb(-8) for value in values], dtype=AMOUNT)


def _amount_arr(arr: np.ndarray) -> pl.Series:
    if arr.size == 0:
        return pl.Series([], dtype=AMOUNT)
    return (pl.Series(arr, dtype=pl.Int64).cast(pl.Decimal(28, 8)) / R).cast(AMOUNT)


def _date_col(days: list[str]) -> pl.Series:
    if not days:
        return pl.Series([], dtype=pl.Date)
    return pl.Series(days, dtype=pl.String).str.to_date("%Y-%m-%d")


def tables(engine, contracts: list) -> dict:
    names = [item["symbol"] for item in contracts]
    owners = [item["run_symbol"] for item in contracts]
    cols = engine.table_columns()
    eq_ts, eq_val, eq_ev = cols["equity"]
    ord_id, ord_ts, ord_day, ord_c, ord_side, ord_off, ord_qty, ord_px, ord_state = cols["orders"]
    fl_id, fl_ord, fl_ts, fl_day, fl_c, fl_side, fl_off, fl_qty, fl_px, fl_fee = cols["fills"]
    pos_day, pos_c, pos_long, pos_short, pos_px, pos_mgn = cols["positions"]
    set_day, set_ts, set_bal, set_eq, set_real, set_fee, set_mgn, set_avail = cols["settlements"]
    ord_c_list = ord_c.tolist()
    fl_c_list = fl_c.tolist()
    pos_c_list = pos_c.tolist()
    return {
        "equity": pl.DataFrame(
            {
                "time_ns": pl.Series(eq_ts, dtype=pl.Int64),
                "equity": _amount_arr(eq_val),
                "event": pl.Series(eq_ev, dtype=pl.String),
            },
            schema=TABLES["equity"],
        ),
        "orders": pl.DataFrame(
            {
                "order": pl.Series(ord_id, dtype=pl.String),
                "time_ns": pl.Series(ord_ts, dtype=pl.Int64),
                "trading_day": _date_col(ord_day),
                "symbol": pl.Series([owners[i] for i in ord_c_list], dtype=pl.String),
                "contract": pl.Series([names[i] for i in ord_c_list], dtype=pl.String),
                "side": pl.Series(ord_side, dtype=pl.String),
                "offset": pl.Series(ord_off, dtype=pl.String),
                "quantity": _amount_arr(ord_qty),
                "price": _amount_arr(ord_px),
                "state": pl.Series(ord_state, dtype=pl.String),
            },
            schema=TABLES["orders"],
        ),
        "fills": pl.DataFrame(
            {
                "fill": pl.Series(fl_id, dtype=pl.String),
                "order": pl.Series(fl_ord, dtype=pl.String),
                "time_ns": pl.Series(fl_ts, dtype=pl.Int64),
                "trading_day": _date_col(fl_day),
                "contract": pl.Series([names[i] for i in fl_c_list], dtype=pl.String),
                "side": pl.Series(fl_side, dtype=pl.String),
                "offset": pl.Series(fl_off, dtype=pl.String),
                "quantity": _amount_arr(fl_qty),
                "price": _amount_arr(fl_px),
                "fee": _amount_arr(fl_fee),
            },
            schema=TABLES["fills"],
        ),
        "positions": pl.DataFrame(
            {
                "trading_day": _date_col(pos_day),
                "contract": pl.Series([names[i] for i in pos_c_list], dtype=pl.String),
                "long": _amount_arr(pos_long),
                "short": _amount_arr(pos_short),
                "settlement_price": _amount_arr(pos_px),
                "margin": _amount_arr(pos_mgn),
            },
            schema=TABLES["positions"],
        ),
        "settlements": pl.DataFrame(
            {
                "trading_day": _date_col(set_day),
                "time_ns": pl.Series(set_ts, dtype=pl.Int64),
                "balance": _amount_arr(set_bal),
                "equity": _amount_arr(set_eq),
                "realized": _amount_arr(set_real),
                "fees": _amount_arr(set_fee),
                "margin": _amount_arr(set_mgn),
                "available": _amount_arr(set_avail),
            },
            schema=TABLES["settlements"],
        ),
    }


STATISTICS = (
    "total_return",
    "max_drawdown",
    "winning_days",
    "annual_return",
    "annual_volatility",
    "sharpe",
    "calmar",
)


def segment(
    config: Config,
    engine,
    first: int,
    last: int,
    base: int,
    begin_ns: int,
    opening: bool,
    *,
    marks=None,
    days=None,
    ends=None,
) -> dict:
    """The statistics of settled days [first, last) from `base` equity."""
    import asterion_engine

    if marks is None:
        marks = engine.equity
    if days is None:
        days = engine.settlements
    # Where each settlement is among the equity observations.
    if ends is None:
        ends = [n for n, point in enumerate(marks) if point[2] == "settlement"]
    values = asterion_engine.performance(
        base,
        [(row[0], row[3]) for row in days[first:last]],
        [point[1] for point in marks[(ends[first - 1] + 1 if first else 0) : ends[last - 1] + 1]],
        begin_ns,
        days[last - 1][1],
        config.annual_periods,
        config.zero_risk_free,
        opening,
    )
    return {name: values[name] for name in STATISTICS}


def segment_metrics(config: Config, engine, split: int, *, marks=None, days=None, ends=None) -> dict:
    """The statistics of the days from index `split` on, from the equity before them."""
    if days is None:
        days = engine.settlements
    return segment(
        config,
        engine,
        split,
        len(days),
        days[split - 1][3],
        days[split - 1][1],
        True,
        marks=marks,
        days=days,
        ends=ends,
    )


def metrics(config: Config, engine) -> dict:
    """The old engine's statistics of the whole run, and with a holdout day of
    the days before it and of the holdout alone."""
    days = engine.settlements
    if not days:
        raise ValueError("the run settled no trading day")
    marks = engine.equity
    ends = [n for n, point in enumerate(marks) if point[2] == "settlement"]
    result = segment(
        config, engine, 0, len(days), raw(config.deposit), marks[0][0], True, marks=marks, days=days, ends=ends
    )
    if config.holdout_day:
        split = next((n for n, row in enumerate(days) if row[0] >= config.holdout_day), len(days))
        if not 0 < split < len(days):
            raise ValueError("the holdout day leaves no days before it or after it")
        for name, value in segment(
            config,
            engine,
            0,
            split,
            raw(config.deposit),
            marks[0][0],
            True,
            marks=marks,
            days=days,
            ends=ends,
        ).items():
            result[f"development_{name}"] = value
        for name, value in segment_metrics(
            config, engine, split, marks=marks, days=days, ends=ends
        ).items():
            result[f"holdout_{name}"] = value
    return result


def costs_record(config: Config) -> dict:
    return {
        "fees": "exchange settlement parameters of each trading day",
        "fee_multiplier": format(config.fee_multiplier.normalize(), "f"),
        "slippage_ticks": config.slippage_ticks,
        "deposit": str(config.deposit),
    }


def risk_record(config: Config) -> dict:
    return {
        "max_gross_weight": str(config.max_gross_weight),
        "max_net_weight": str(config.max_net_weight),
        "max_margin_ratio": str(config.max_margin_ratio),
        "min_rebalance_weight": str(config.min_rebalance_weight),
        "max_order_lots": str(config.max_order_lots),
        "max_gross_lots": str(config.max_gross_lots),
        "max_contract_lots": str(config.max_contract_lots),
        "max_working_orders": config.max_working_orders,
    }


def statistics_record(config: Config) -> dict:
    return {
        "annual_periods": config.annual_periods,
        "zero_risk_free": config.zero_risk_free,
        "holdout_day": config.holdout_day,
    }


def held_without_prices(engine, contracts: list):
    """A position held where its month has no prices could neither close nor roll."""
    unpriced = [set(item["unpriced"]) for item in contracts]
    if not any(unpriced):
        return
    held = {}
    for row in engine.positions:
        index = row[1]
        if row[0] in unpriced[index]:
            held.setdefault(index, []).append(row[0])
    for index, item in enumerate(contracts):
        days = held.get(index)
        if days:
            raise ValueError(
                f"prices of {item['symbol']} are missing on "
                f"{spans(days, item['unpriced'])}, while it is held"
            )


PIT_TOKENS = ("receipt_volume", "member_long", "member_short", "inventory_total", "factors.fundamentals")


def uses_pit_fundamentals(source_root: str) -> bool:
    if not source_root:
        return False
    root = Path(source_root)
    if not root.is_dir():
        return False
    for path in root.rglob("*.py"):
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if any(token in text for token in PIT_TOKENS):
            return True
    return False


def strategy_lineage(strategy) -> dict | None:
    lineage = {}
    evaluation = getattr(strategy, "source_evaluation", None)
    if isinstance(evaluation, str) and evaluation.strip():
        lineage["factor_evaluation"] = evaluation.strip()
    source_runs = getattr(strategy, "source_runs", None)
    if isinstance(source_runs, (list, tuple)):
        ids = [item.strip() for item in source_runs if isinstance(item, str) and item.strip()]
        if ids:
            lineage["source_runs"] = ids
    return lineage or None


def replay_record(
    engine,
    contracts: list,
    minutes: int,
    *,
    run_diagnostics: dict | None = None,
    lineage: dict | None = None,
) -> dict:
    data = {"rejections": rejections_record(engine, contracts)}
    if run_diagnostics is not None:
        data["diagnostics"] = run_diagnostics
    if lineage:
        data["lineage"] = lineage
    if minutes:
        days = [day for item in contracts for day in item["excluded"]]
        data["excluded"] = {"bars": len(days), "days": sorted(set(days))}
        data["minute_coverage"] = [item["minute_coverage"] for item in contracts]
    return data


def rejections_record(engine, contracts: list) -> list:
    """Targets the risk limits or funds refused, one row per contract, lots and
    reason with how often and when. Trading ends an instance on the first."""
    rows = {}
    for time_ns, _, contract, lots, reason in engine.rejections:
        key = (contracts[contract]["symbol"], str(Decimal(lots).scaleb(-8).normalize()), reason)
        row = rows.setdefault(
            key,
            {
                "contract": key[0],
                "lots": key[1],
                "reason": reason,
                "count": 0,
                "first_ms": time_ns // 10**6,
            },
        )
        row["count"] += 1
        row["last_ms"] = time_ns // 10**6
    return list(rows.values())


def bars_record(config: Config) -> dict:
    return {"period": f"{config.minutes}m" if config.minutes else "day"}


def plan(config: Config, version: str) -> Plan:
    return Plan(
        repository=config.repository,
        strategy=config.strategy,
        revision=repository.Revision(config.commit, config.changes),
        params=config.params,
        dataset_version=version,
        symbols=tuple(config.symbols),
        begin_day=config.begin_day,
        end_day=config.end_day,
        costs=costs_record(config),
        risk=risk_record(config),
        statistics=statistics_record(config),
        bars=bars_record(config),
    )


def run(directory: Path, identity: str, config: Config, started=lambda: None, *, cache: dict | None = None):
    """One backtest from start to its record. A failure is recorded as failed;
    `started` is called once the record exists."""
    import asterion_engine

    runs = Runs(directory)
    if not config.source:
        raise ValueError("research runs read the source copied at admission")
    data_key = (config.dataset_version, config.begin_day, config.end_day)
    if cache is not None and cache.get("data_key") == data_key:
        data = cache["data"]
    else:
        data = open_data(directory, config)
        if cache is not None:
            cache["data_key"] = data_key
            cache["data"] = data
            cache.pop("prep_key", None)
            cache.pop("prepared", None)
    runs.start(identity, plan(config, data.version.identity))
    started()
    try:
        strategy = repository.load(Path(config.source), config.strategy, config.params)
        prep_key = (
            config.dataset_version,
            config.symbols,
            config.begin_day,
            config.end_day,
            config.minutes,
            config.fee_multiplier,
            config.costs,
        )
        if cache is not None and cache.get("prep_key") == prep_key:
            cached_contracts, series, first = cache["prepared"]
            if cached_contracts and cached_contracts[0]["slippage_ticks"] != config.slippage_ticks:
                contracts = [{**item, "slippage_ticks": config.slippage_ticks} for item in cached_contracts]
            else:
                contracts = cached_contracts
        else:
            contracts, series, first = prepare(data, config)
            _pack_bar_columns(contracts)
            if cache is not None:
                cache["prep_key"] = prep_key
                cache["prepared"] = (contracts, series, first)
        engine = asterion_engine.Replay(
            raw(config.deposit),
            contracts,
            series,
            (
                raw(config.max_order_lots),
                raw(config.max_gross_lots),
                config.max_working_orders,
                raw(config.max_contract_lots),
            ),
        )
        symbols = {engine.unit(index): symbol for symbol, index in first.items()}
        tracker = (
            diagnostics.ComponentTracker(strategy, rolls(contracts))
            if isinstance(strategy, CombinedStrategy)
            else None
        )
        replay(
            engine,
            strategy,
            symbols,
            contracts,
            config,
            decided=tracker.observe if tracker is not None else (lambda bars, wanted: None),
        )
        held_without_prices(engine, contracts)
        run_tables = tables(engine, contracts)
        if cache is not None:
            pit_flags = cache.setdefault("uses_pit", {})
            if config.source not in pit_flags:
                pit_flags[config.source] = uses_pit_fundamentals(config.source)
            has_pit = pit_flags[config.source]
        else:
            has_pit = uses_pit_fundamentals(config.source)
        run_diag = diagnostics.compute(
            config,
            contracts,
            run_tables,
            tracker.summary() if tracker is not None else None,
            series=series,
            trials=runs.strategy_trials(config.strategy, config.holdout_day),
            uses_pit_fundamentals=has_pit,
        )
        runs.succeed(
            identity,
            run_tables,
            metrics(config, engine),
            replay_record(
                engine,
                contracts,
                config.minutes,
                run_diagnostics=run_diag,
                lineage=strategy_lineage(strategy),
            ),
        )
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

    minutes = {
        Period.PERIOD_DAY: 0,
        Period.PERIOD_MINUTES_1: 1,
        Period.PERIOD_MINUTES_5: 5,
        Period.PERIOD_MINUTES_10: 10,
        Period.PERIOD_MINUTES_15: 15,
        Period.PERIOD_MINUTES_30: 30,
        Period.PERIOD_MINUTES_60: 60,
    }
    if period not in minutes:
        raise ValueError(f"backtest bars are daily or {', '.join(map(str, chart.MINUTES))} minutes")
    return minutes[period]


def config_of(admitted) -> Config:
    """A Config from an AdmittedRun message."""
    request = admitted.backtest
    if request.first_day and request.last_day and request.first_day > request.last_day:
        raise ValueError("backtest first_day must not be after last_day")
    params = {
        name: getattr(value, value.WhichOneof("value"))
        for name, value in request.params.items()
        if value.WhichOneof("value")
    }
    limits = request.limits
    fee_multiplier = number("fee_multiplier", request.fee_multiplier.text or "1")
    if fee_multiplier < 0:
        raise ValueError("fee_multiplier must be nonnegative")
    return Config(
        repository=request.repository,
        strategy=request.strategy,
        params=params,
        symbols=tuple(request.symbols),
        begin_day=request.first_day,
        end_day=request.last_day,
        deposit=number("deposit", request.deposit.text),
        costs=None,
        slippage_ticks=request.slippage_ticks,
        max_order_lots=number("max_order_lots", limits.max_order_lots.text),
        max_gross_lots=number("max_gross_lots", limits.max_gross_lots.text),
        max_contract_lots=number("max_contract_lots", limits.max_contract_lots.text),
        max_working_orders=limits.max_working_orders,
        max_gross_weight=number("max_gross_weight", request.max_gross_weight.text or "0"),
        max_net_weight=number("max_net_weight", request.max_net_weight.text or "0"),
        max_margin_ratio=number("max_margin_ratio", request.max_margin_ratio.text or "0"),
        min_rebalance_weight=number("min_rebalance_weight", request.min_rebalance_weight.text or "0"),
        fee_multiplier=fee_multiplier,
        annual_periods=request.periods_per_year or None,
        zero_risk_free=request.zero_risk_free_rate,
        holdout_day=request.holdout_day,
        minutes=minutes_of(request.period),
        dataset_version=admitted.pinned.dataset_version,
        source=admitted.source,
        commit=admitted.pinned.commit,
        changes=admitted.pinned.changes,
    )


def main():
    import os
    from asterion.lab_pb2 import AdmittedRun
    from asterion_lab import worker

    if len(sys.argv) >= 3 and sys.argv[1] == "--worker":
        output = os.fdopen(os.dup(1), "wb")
        os.dup2(2, 1)
        directory = Path(sys.argv[2])
        cache: dict = {}
        current_source: str | None = None
        while (payload := worker.read_frame(sys.stdin.buffer)) is not None:
            request = AdmittedRun()
            request.ParseFromString(payload)
            try:
                current_source = worker.switch_source(current_source, request.source)
                run(
                    directory,
                    request.backtest.id,
                    config_of(request),
                    lambda: worker.write_frame(output, b"S"),
                    cache=cache,
                )
                worker.write_frame(output, b"D")
            except Exception as error:
                import traceback

                traceback.print_exc()
                sys.stderr.flush()
                worker.write_frame(output, b"E" + (str(error) or type(error).__name__).encode())
        return
    request = AdmittedRun()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(
            Path(sys.argv[1]),
            request.backtest.id,
            config_of(request),
            lambda: print("started", flush=True),
        )
    except BaseException:
        import traceback

        traceback.print_exc()
        sys.exit(1)
