"""Sweep planning, Bayesian wave advancement, CSCV-PBO and sweep status summaries."""
from dataclasses import dataclass
import itertools
import math

from asterion.common_pb2 import Period
from asterion.lab_pb2 import AdmittedRun, RollingRun

from asterion_lab import backtest, data as lab_data, optimize, rolling, statistics as lab_stats
from asterion_lab.runs import BATCH_STRATEGY, IDENTITY, SLIPPAGE_STRESS


@dataclass
class Rolling:
    request: object  # RollingRun
    candidates: int
    starting: bool = False


@dataclass
class BayesSweep:
    sweep_id: str
    base: object  # AdmittedRun
    names: list
    axis_values: list  # list[list[ParameterValue]]
    axis_coords: list  # list[list[float]]
    max_trials: int
    seed: int
    limit: int
    next_index: int
    target_metric: str
    planned_combos: dict  # run_id -> tuple[int, ...]
    observed: dict  # tuple[int, ...] -> float
    tried: set  # set[tuple[int, ...]]
    advancing: bool = False


def materialize_combo(
    base,
    sweep_id: str,
    index: int,
    names: list,
    axis_values: list,
    combo: tuple[int, ...],
):
    run = AdmittedRun()
    run.CopyFrom(base)
    run.backtest.id = f"{sweep_id}-{index}"
    swept = {}
    for d, name in enumerate(names):
        val = axis_values[d][combo[d]]
        run.backtest.params[name].CopyFrom(val)
        swept[name] = getattr(val, val.WhichOneof("value"))
    return run, (run.backtest.id, swept)


def validate_sweep(request, waiting) -> dict:
    """Validate a StartSweep request and return parsed dimensions before pinning data."""
    if not IDENTITY.fullmatch(request.id) or any(item[0] == request.id for item in waiting):
        raise ValueError("sweep id is invalid or already used")
    symbols = list(request.symbols)
    if len(set(symbols)) != len(symbols) or any(not item.strip() for item in symbols):
        raise ValueError("a sweep lists distinct non-empty symbols")
    names = [item.name for item in request.values]
    if len(set(names)) != len(names) or any(not item.values for item in request.values):
        raise ValueError("a sweep gives each of its parameters one or more values")
    batch = list(request.strategies)
    slippages = list(request.slippage_ticks)
    if batch and (names or symbols or slippages or request.HasField("walk_forward")):
        raise ValueError("a strategy batch varies only the strategy")
    if batch and (len(batch) < 2 or len({item.strategy for item in batch}) != len(batch)):
        raise ValueError("a strategy batch runs two or more distinct strategies")
    if slippages and (names or symbols or batch or request.HasField("walk_forward")):
        raise ValueError("a slippage stress sweep varies only slippage_ticks")
    if slippages and (len(slippages) < 2 or len(set(slippages)) != len(slippages)):
        raise ValueError("a slippage stress sweep runs two or more distinct tick levels")
    if not batch and not slippages and not names and len(symbols) < 2:
        raise ValueError("a sweep varies parameters, slippage or two or more symbols")
    search_mode = request.search_mode or "grid"
    if search_mode not in ("grid", "lhs", "bayes"):
        raise ValueError(f"unknown sweep search_mode: {search_mode!r}")
    if search_mode != "grid" and (batch or slippages or len(symbols) > 1 or not names):
        raise ValueError(f"{search_mode} search requires parameter values on a single symbol set")
    symbol_items = symbols or [None]
    slippage_items = slippages or [None]
    axis_values = [list(item.values) for item in request.values]
    axis_lengths = [len(vals) for vals in axis_values]
    total_space = len(symbol_items) * len(slippage_items) * math.prod(axis_lengths)
    max_trials = 0
    if search_mode == "grid":
        if total_space > 10000:
            raise ValueError(f"a sweep runs at most 10000 combinations, not {total_space}")
    else:
        max_trials = min(request.max_trials or 32, total_space)
        if max_trials < 2 or max_trials > 10000:
            raise ValueError("lhs and bayes sweeps require 2 to 10000 trials")
    if request.HasField("walk_forward") and len(symbols) > 1:
        raise ValueError("rolling validation compares parameter values on the same symbols")
    backtest.config_of(AdmittedRun(backtest=request.base))
    return dict(
        symbols=symbols,
        symbol_items=symbol_items,
        names=names,
        batch=batch,
        slippages=slippages,
        slippage_items=slippage_items,
        search_mode=search_mode,
        axis_values=axis_values,
        axis_lengths=axis_lengths,
        max_trials=max_trials,
    )


def plan_runs(request, base, found, limit: int, parsed: dict):
    """Build the initial wave of AdmittedRun objects, sweep plan rows and optional BayesSweep."""
    seed = int(request.seed) if request.seed else optimize.DEFAULT_SEED
    is_minutes = request.base.period != Period.PERIOD_DAY
    names = parsed["names"]
    batch = parsed["batch"]
    axis_values = parsed["axis_values"]
    axis_lengths = parsed["axis_lengths"]
    search_mode = parsed["search_mode"]
    max_trials = parsed["max_trials"]
    bayes_state = None
    runs, planned = [], []

    if search_mode == "grid":
        combinations = list(itertools.product(*axis_values)) if names else [()]
        for symbol in parsed["symbol_items"]:
            span = (
                lab_data.stored_span(found, [symbol], is_minutes)
                if symbol and found is not None
                else None
            )
            for values, choice, slip in itertools.product(
                combinations, batch or [None], parsed["slippage_items"]
            ):
                n = len(runs)
                run = AdmittedRun()
                run.CopyFrom(base)
                run.backtest.id = f"{request.id}-{n}"
                if symbol is not None:
                    del run.backtest.symbols[:]
                    run.backtest.symbols.append(symbol)
                    if span is not None:
                        if not run.backtest.first_day:
                            run.backtest.first_day = span[0]
                        if not run.backtest.last_day:
                            run.backtest.last_day = span[1]
                for name, value in zip(names, values):
                    run.backtest.params[name].CopyFrom(value)
                swept = {
                    name: getattr(value, value.WhichOneof("value")) for name, value in zip(names, values)
                }
                if choice:
                    run.backtest.strategy = choice.strategy
                    run.backtest.params.clear()
                    for name, value in choice.params.items():
                        run.backtest.params[name].CopyFrom(value)
                    swept[BATCH_STRATEGY] = choice.strategy
                if slip is not None:
                    run.backtest.slippage_ticks = slip
                    swept[SLIPPAGE_STRESS] = slip
                runs.append(run)
                planned.append((run.backtest.id, swept))
    elif search_mode == "lhs":
        sampled = optimize.lhs_sample(axis_lengths, max_trials, seed=seed)
        for idx, combo in enumerate(sampled):
            run, planned_item = materialize_combo(base, request.id, idx, names, axis_values, combo)
            runs.append(run)
            planned.append(planned_item)
    else:
        init_n = min(max_trials, max(4, min(limit * 2, len(names) * 2 + 2)))
        initial_combos = optimize.lhs_sample(axis_lengths, init_n, seed=seed)
        planned_combos = {}
        for idx, combo in enumerate(initial_combos):
            run, planned_item = materialize_combo(base, request.id, idx, names, axis_values, combo)
            runs.append(run)
            planned.append(planned_item)
            planned_combos[run.backtest.id] = combo
        axis_coords = [
            optimize.encode_axis([getattr(v, v.WhichOneof("value")) for v in vals])
            for vals in axis_values
        ]
        target_metric = "development_sharpe" if request.base.holdout_day else "sharpe"
        bayes_state = BayesSweep(
            sweep_id=request.id,
            base=base,
            names=names,
            axis_values=axis_values,
            axis_coords=axis_coords,
            max_trials=max_trials,
            seed=seed,
            limit=limit,
            next_index=len(runs),
            target_metric=target_metric,
            planned_combos=planned_combos,
            observed={},
            tried=set(initial_combos),
        )
    return runs, planned, bayes_state


def build_rolling_run(request, base, runs: list, bayes_state: BayesSweep | None):
    if not request.HasField("walk_forward"):
        return None
    if request.base.holdout_day or (len(runs) < 2 and not bayes_state):
        raise ValueError("rolling validation compares two or more candidates and takes no holdout")
    if not request.base.periods_per_year or not request.base.zero_risk_free_rate:
        raise ValueError("rolling validation requires periods_per_year and zero_risk_free_rate")
    if request.walk_forward.training_days < 20 or request.walk_forward.validation_days < 1:
        raise ValueError("rolling validation needs at least 20 training days and one validation day")
    rolling.normalize_objective(request.walk_forward.objective)
    return RollingRun(
        id=f"{request.id}-rolling",
        sweep_id=request.id,
        base=base,
        walk_forward=request.walk_forward,
    )


def observe_bayes_rows(rows: list, bayes: BayesSweep):
    for row in rows:
        combo = bayes.planned_combos.get(row["id"])
        if combo is None or combo in bayes.observed or row["state"] == "running":
            continue
        score = row["metrics"].get(bayes.target_metric)
        if score is None:
            score = row["metrics"].get("total_return")
        if score is None or not math.isfinite(float(score)):
            score = -10.0
        bayes.observed[combo] = float(score)


def materialize_bayes_wave(sweep: str, bayes: BayesSweep, next_combos: list):
    offset = bayes.next_index
    new_runs, new_planned = [], []
    for pos, combo in enumerate(next_combos):
        run, planned_item = materialize_combo(
            bayes.base, sweep, offset + pos, bayes.names, bayes.axis_values, combo
        )
        backtest.config_of(run)
        new_runs.append(run)
        new_planned.append(planned_item)
        bayes.planned_combos[run.backtest.id] = combo
        bayes.tried.add(combo)
    bayes.next_index += len(new_runs)
    return offset, new_runs, new_planned


def compute_sweep_pbo(runs_store, rows: list) -> dict | None:
    succeeded = [
        row
        for row in rows
        if row["state"] == "succeeded"
        and BATCH_STRATEGY not in row["params"]
        and SLIPPAGE_STRESS not in row["params"]
    ]
    if len(succeeded) < 2:
        return None
    series_by_run = []
    common_days = None
    annual_periods = 252.0
    for row in succeeded:
        try:
            record = runs_store.record(row["id"])
            deposit = float(record["costs"]["deposit"])
            if deposit <= 0:
                continue
            ann = record.get("statistics", {}).get("annual_periods")
            if ann:
                annual_periods = float(ann)
            settlements = runs_store.table(row["id"], "settlements")
            days = [d.isoformat() for d in settlements["trading_day"]]
            equities = [float(e) for e in settlements["equity"]]
        except (OSError, KeyError, ValueError):
            continue
        if len(days) < 16:
            continue
        rets = {}
        prev = deposit
        for d_str, eq in zip(days, equities):
            rets[d_str] = (eq / prev - 1.0) if prev > 0 else 0.0
            prev = eq
        series_by_run.append(rets)
        day_set = set(rets)
        common_days = day_set if common_days is None else (common_days & day_set)
    if len(series_by_run) < 2 or not common_days or len(common_days) < 16:
        return None
    ordered_days = sorted(common_days)
    matrix = [[s[d] for s in series_by_run] for d in ordered_days]
    return lab_stats.cscv_pbo(matrix, annual_periods=annual_periods)


def sweep_info(runs_store, sweep_id: str, meta: dict, running: int, queued: int) -> dict:
    rows = list(runs_store.sweep(sweep_id))
    if meta["has_rolling"]:
        rolling_row = runs_store.get(f"{sweep_id}-rolling")
        if rolling_row is not None:
            rows.append(rolling_row)
    succeeded = sum(1 for row in rows if row["state"] == "succeeded")
    failed_rows = [row for row in rows if row["state"] in ("failed", "interrupted", "stopped")]
    failed = len(failed_rows)
    first_error = next((row["error"] for row in failed_rows if row.get("error")), "")
    total = max(meta["total"], len(rows))
    done = succeeded + failed
    if running or queued:
        state = "running"
    elif succeeded == total and total > 0:
        state = "succeeded"
    elif any(row["state"] == "interrupted" for row in failed_rows) or done < total:
        state = "interrupted"
    else:
        state = "failed"
    return dict(
        id=sweep_id,
        kind=meta["kind"],
        strategy=meta["strategy"],
        symbols=meta["symbols"],
        total=total,
        done=done,
        succeeded=succeeded,
        failed=failed,
        running=running,
        queued=queued,
        started_ms=meta["started_ms"],
        finished_ms=0,
        state=state,
        error="" if state == "succeeded" else first_error,
    )
