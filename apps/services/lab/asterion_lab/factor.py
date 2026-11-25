"""A factor evaluated across symbols, in a child process of the lab service.

A factor is a plain function of the strategy repository (see repository.py):

    def momentum(close, lookback: int = 20):
        \"\"\"Return over the last `lookback` bars.\"\"\"
        result = np.full(len(close), np.nan)
        result[lookback:] = close[lookback:] / close[:-lookback] - 1
        return result

It is called once per symbol with read-only arrays of that symbol's bars, in
order: exactly what a strategy's ctx.history returns at the last bar, a
dominant series at the level of its latest month. It returns one value per bar, NaN where it has
none; the value of a bar may use that bar and those before it, never later
ones, which is also when a strategy could act on it.

Each day, the symbols with a value are ranked against their returns from that
close to the close `horizon` bars later (Spearman's rank correlation, the IC),
and split into `groups` equal groups by value, whose next-bar returns are
averaged. The child reads an AdmittedFactor on standard input, imports the
factor from the copy of the repository taken at admission, and writes a
FactorResult to standard output:

    python3 -I -B -c "...; from asterion_lab.factor import main; main()" <lab-directory>
"""
from bisect import bisect_left
from decimal import Decimal
import math
import os
from pathlib import Path
import sys

import numpy as np
import polars as pl

from asterion_lab import repository
from asterion_lab.backtest import COSTS, Config, Data, observations, prepare, raw, rolls
from asterion_lab.warehouse_client import Dataset
from asterion_strategy import (
    FIELDS,
    KINDS,
    Context,
    observe,
    orthogonalize,
    sector_neutralize,
    verify_factor_causality,
    winsorize_mad,
)

MINIMUM = 3  # symbols a day's IC needs


def symbols_of(data: Data, symbols, prepared: dict | None = None) -> tuple:
    """The symbols to evaluate: an exchange stands for each of its products
    with daily bars in the range. Returns them and the products left out,
    each with why: those that cannot form a series."""
    chosen, skipped = [], []
    with data.warehouse:
        counts = {}
        for symbol in symbols:
            parts = symbol.split("/")
            if len(parts) in (2, 3):
                counts.setdefault(parts[0], set()).add(parts[1])
        for exchange, products in counts.items():
            if len(products) > 1:
                data.load_exchange(exchange)
        for symbol in symbols:
            if "/" in symbol:
                chosen.append(symbol)
                continue
            data.check_stored(symbol)
            for product in data.load_exchange(symbol):
                name = f"{symbol}/{product}"
                try:
                    ready = prepare(data, config(data, (name,)))
                    if prepared is not None:
                        prepared[name] = ready
                    chosen.append(name)
                except ValueError as error:
                    skipped.append((name, str(error)))
        if len(set(chosen)) != len(chosen):
            raise ValueError("a symbol is given twice")
        if prepared is not None:
            for symbol in chosen:
                if symbol not in prepared:
                    prepared[symbol] = prepare(data, config(data, (symbol,)))
    return tuple(chosen), skipped


def config(data: Data, symbols) -> Config:
    """A replay that trades nothing: costs and limits play no part, but the
    account accepts only valid ones."""
    costs = [Decimal(1) if name == "margin_per_lot" else Decimal(0) for name in COSTS]
    return Config(repository="", strategy="", params={}, symbols=tuple(symbols),
                  begin_day=data.begin, end_day=data.end, deposit=Decimal(1),
                  costs=costs,
                  slippage_ticks=0, max_order_lots=Decimal(0), max_gross_lots=Decimal(0),
                  max_contract_lots=Decimal(0), max_working_orders=0)


def bars(data: Data, symbol: str, prepared=None) -> dict:
    """A symbol's bars as a strategy sees them: its days and an array of each
    field. Each symbol is replayed alone, so a contract and the series it is a
    month of can both be evaluated."""
    import asterion_engine

    contracts, series, _ = prepared or prepare(data, config(data, (symbol,)))
    limit = raw(Decimal(1))
    engine = asterion_engine.Replay(limit, contracts, series, (limit, limit, 1, limit))
    context, days, segments, start = Context([symbol], rolls(contracts)), [], [], 0
    for completed in observations(engine, {0: symbol}, contracts):
        if completed:
            if context.contracts.get(symbol, completed[0].contract) != completed[0].contract:
                segments.append((start, {field: context.history(symbol, field, len(days))
                                         for field in FIELDS}))
                start = len(days)
            observe(context, completed)
            days.extend(bar.time.date().isoformat() for bar in completed)
    snapshot = {field: context.history(symbol, field, len(days)) for field in FIELDS}
    segments.append((start, snapshot))
    return {"day": days, "segments": segments, **snapshot}


def values(function, inputs, settings, symbols: dict) -> pl.DataFrame:
    """The factor's value and the close of every symbol's bars."""
    frames = []
    for symbol, columns in symbols.items():
        count = len(columns["day"])
        segments = columns.get("segments") or [(0, columns)]
        parts = []
        for start, snapshot in segments:
            end = len(snapshot["close"])
            try:
                computed = function(**{name: snapshot[name] for name in inputs}, **settings)
                part = np.asarray(computed, dtype=float)
            except Exception as error:
                raise ValueError(f"the factor failed on {symbol}: {type(error).__name__}: {error}")
            if part.shape != (end,):
                raise ValueError(
                    f"the factor gave {part.shape} values for the {end} bars of {symbol}")
            parts.append(part[start:end])
        result = np.concatenate(parts) if parts else np.empty(0, dtype=float)
        opening = columns["open"] if "open" in columns else columns["close"]
        vol = columns["volume"] if "volume" in columns else np.full(count, np.nan)
        oi = columns["open_interest"] if "open_interest" in columns else np.full(count, np.nan)
        frames.append(pl.DataFrame({
            "symbol": [symbol] * count, "day": columns["day"], "bar": np.arange(count),
            "open": opening, "close": columns["close"], "volume": vol,
            "open_interest": oi, "value": result}))
    return pl.concat(frames).with_columns(
        pl.when(pl.col("value").is_finite()).then(pl.col("value")).otherwise(None))


def filter_liquidity(frame: pl.DataFrame, min_volume: float = 0.0,
                     min_open_interest: float = 0.0, window: int = 20) -> pl.DataFrame:
    """Mask factor values on symbol-days below trailing mean volume or open-interest floors."""
    if not math.isfinite(min_volume) or min_volume < 0:
        raise ValueError("min_volume must be a non-negative finite number")
    if not math.isfinite(min_open_interest) or min_open_interest < 0:
        raise ValueError("min_open_interest must be a non-negative finite number")
    if (min_volume == 0.0 and min_open_interest == 0.0) or frame.is_empty():
        return frame
    ordered = frame.sort("symbol", "day")
    keep = pl.lit(True)
    if min_volume > 0.0 and "volume" in ordered.columns:
        mean_vol = (
            pl.when(pl.col("volume").is_finite()).then(pl.col("volume")).otherwise(None)
            .rolling_mean(window_size=window, min_samples=1)
            .over("symbol")
        )
        keep = keep & mean_vol.is_not_null() & (mean_vol >= min_volume)
    if min_open_interest > 0.0 and "open_interest" in ordered.columns:
        mean_oi = (
            pl.when(pl.col("open_interest").is_finite()).then(pl.col("open_interest")).otherwise(None)
            .rolling_mean(window_size=window, min_samples=1)
            .over("symbol")
        )
        keep = keep & mean_oi.is_not_null() & (mean_oi >= min_open_interest)
    return ordered.with_columns(
        pl.when(keep).then(pl.col("value")).otherwise(None).alias("value")
    )


def _apply_daily(frame: pl.DataFrame, transform) -> pl.DataFrame:
    """Apply a per-day cross-sectional `{symbol: value}` transform to `frame`."""
    if frame.is_empty():
        return frame
    valid = frame.filter(pl.col("value").is_finite())
    if valid.is_empty():
        return frame.with_columns(pl.lit(None, dtype=pl.Float64).alias("value"))
    rows = []
    for (day,), part in valid.group_by("day", maintain_order=True):
        scores = dict(zip(part["symbol"].to_list(), part["value"].to_list()))
        for symbol, value in transform(scores).items():
            if math.isfinite(value):
                rows.append((day, symbol, float(value)))
    if not rows:
        return frame.with_columns(pl.lit(None, dtype=pl.Float64).alias("value"))
    updated = pl.DataFrame(rows, schema={"day": pl.String, "symbol": pl.String, "value": pl.Float64}, orient="row")
    return frame.drop("value").join(updated, on=["day", "symbol"], how="left")


def orthogonalize_frame(frame: pl.DataFrame, controls: list) -> pl.DataFrame:
    """Replace each day's factor values with OLS residuals against `controls`."""
    if not controls or frame.is_empty():
        return frame
    control_days = []
    for ctrl in controls:
        by_day = {}
        valid = ctrl.filter(pl.col("value").is_finite())
        for (day,), part in valid.group_by("day"):
            by_day[day] = dict(zip(part["symbol"].to_list(), part["value"].to_list()))
        control_days.append(by_day)
    valid = frame.filter(pl.col("value").is_finite())
    rows = []
    for (day,), part in valid.group_by("day", maintain_order=True):
        scores = dict(zip(part["symbol"].to_list(), part["value"].to_list()))
        day_controls = [by_day.get(day, {}) for by_day in control_days]
        for symbol, value in orthogonalize(scores, day_controls).items():
            if math.isfinite(value):
                rows.append((day, symbol, float(value)))
    if not rows:
        return frame.with_columns(pl.lit(None, dtype=pl.Float64).alias("value"))
    updated = pl.DataFrame(rows, schema={"day": pl.String, "symbol": pl.String, "value": pl.Float64}, orient="row")
    return frame.drop("value").join(updated, on=["day", "symbol"], how="left")


def preprocess_frame(frame: pl.DataFrame, *, winsorize: bool = False, standardize: str = "",
                     neutralize: str = "", controls: list | None = None) -> pl.DataFrame:
    """Cross-sectional cleaning pipeline: MAD clip -> standardize -> neutralize -> orthogonalize."""
    if neutralize not in ("", "none", "sector"):
        raise ValueError(f"unknown factor neutralization: {neutralize!r}")
    if winsorize:
        frame = _apply_daily(frame, winsorize_mad)
    frame = standardize_frame(frame, standardize)
    if neutralize == "sector":
        frame = _apply_daily(frame, sector_neutralize)
    if controls:
        frame = orthogonalize_frame(frame, controls)
    return frame


def standardize_frame(frame: pl.DataFrame, method: str) -> pl.DataFrame:
    """Cross-sectionally transform each day's factor values ('none', 'zscore' or 'rank')."""
    if method in ("", "none"):
        return frame
    if method not in ("zscore", "rank"):
        raise ValueError(f"unknown factor standardization: {method!r}")
    if frame.is_empty():
        return frame
    value = pl.when(pl.col("value").is_finite()).then(pl.col("value")).otherwise(None)
    count = value.count().over("day")
    if method == "zscore":
        mean_val = value.mean().over("day")
        std_val = value.std(ddof=1).over("day")
        expr = (
            pl.when(value.is_null() | (count < 2))
            .then(None)
            .when((std_val > 0) & std_val.is_finite())
            .then(((value - mean_val) / std_val).clip(-3.0, 3.0))
            .otherwise(0.0)
        )
    else:
        ranked = value.rank(method="average").over("day") - 1.0
        expr = (
            pl.when(value.is_null() | (count < 2))
            .then(None)
            .otherwise(2.0 * ranked / (count - 1) - 1.0)
        )
    return frame.sort("day", "symbol").with_columns(expr.cast(pl.Float64).alias("value"))


def factor_correlations(frames: list) -> list:
    """Mean daily Spearman rank correlation matrix (K x K flattened) across K factors."""
    k = len(frames)
    if k < 2:
        return []
    joined = frames[0].select("symbol", "day", pl.col("value").alias("v0"))
    for idx in range(1, k):
        joined = joined.join(
            frames[idx].select("symbol", "day", pl.col("value").alias(f"v{idx}")),
            on=["symbol", "day"],
            how="full",
            coalesce=True,
        )
    matrix = [None] * (k * k)
    for i in range(k):
        for j in range(i, k):
            pair = joined.filter(pl.col(f"v{i}").is_finite() & pl.col(f"v{j}").is_finite())
            daily = (
                pair.group_by("day")
                .agg(
                    pl.len().alias("count"),
                    pl.corr(f"v{i}", f"v{j}", method="spearman").alias("corr"),
                )
                .filter((pl.col("count") >= MINIMUM) & pl.col("corr").is_finite())
            )
            corr = {"days": daily.height, "samples": int(daily["count"].sum())}
            if daily.height:
                corr["value"] = float(daily["corr"].mean())
            matrix[i * k + j] = corr
            matrix[j * k + i] = corr
    return matrix


def combine_factors(frames: list, method: str, horizon: int, standardize: str = "",
                    return_mode: str = "") -> pl.DataFrame:
    """Combine multiple factor frames by 'equal' or trailing 'ic_weighted' weights."""
    if method not in ("", "equal", "ic_weighted"):
        raise ValueError(f"unknown factor combination: {method!r}")
    if return_mode not in ("", "close", "open"):
        raise ValueError(f"unknown factor return mode: {return_mode!r}")
    norm = standardize if standardize in ("zscore", "rank") else "zscore"
    std_frames = [standardize_frame(f, norm) for f in frames]
    k = len(std_frames)
    meta_cols = [c for c in ("symbol", "day", "bar", "open", "close", "volume", "open_interest")
                 if c in std_frames[0].columns]
    base = std_frames[0].select(*meta_cols, pl.col("value").alias("v0"))
    for idx in range(1, k):
        base = base.join(
            std_frames[idx].select("symbol", "day", pl.col("value").alias(f"v{idx}")),
            on=["symbol", "day"],
            how="left",
        )
    days = sorted(set(base["day"]))
    day_to_idx = {d: i for i, d in enumerate(days)}
    if method in ("", "equal"):
        cols = [pl.col(f"v{idx}") for idx in range(k)]
        return (
            base.with_columns(pl.mean_horizontal(*cols).alias("value"))
            .select(*meta_cols, "value")
            .with_columns(pl.when(pl.col("value").is_finite()).then(pl.col("value")).otherwise(None))
        )
    # Trailing IC-weighted combination: day t uses only forward returns fully realized by bar t.
    indexed = base.with_columns(pl.col("day").replace_strict(day_to_idx).alias("day_index"))
    if return_mode == "open" and "open" in base.columns:
        entry = indexed.select("symbol", (pl.col("day_index") - 1).alias("day_index"),
                               pl.col("open").alias("entry_open"))
        exit_ = indexed.select("symbol", (pl.col("day_index") - horizon - 1).alias("day_index"),
                               pl.col("open").alias("exit_open"))
        indexed = (
            indexed.join(entry, on=["symbol", "day_index"], how="left")
            .join(exit_, on=["symbol", "day_index"], how="left")
            .with_columns(
                pl.when(pl.col("entry_open") > 0)
                .then(pl.col("exit_open") / pl.col("entry_open") - 1)
                .otherwise(None)
                .alias("ahead")
            )
            .drop("entry_open", "exit_open")
        )
        realized_lag = horizon + 1
    else:
        future = indexed.select("symbol", (pl.col("day_index") - horizon).alias("day_index"),
                                pl.col("close").alias("future_close"))
        indexed = (
            indexed.join(future, on=["symbol", "day_index"], how="left")
            .with_columns((pl.col("future_close") / pl.col("close") - 1).alias("ahead"))
            .drop("future_close")
        )
        realized_lag = horizon
    daily_ics = [{} for _ in range(k)]
    for idx in range(k):
        valid = indexed.filter(pl.col(f"v{idx}").is_not_null() & pl.col("ahead").is_finite())
        agg = (
            valid.group_by("day_index")
            .agg(pl.len().alias("count"), pl.corr(f"v{idx}", "ahead", method="spearman").alias("ic"))
            .filter((pl.col("count") >= MINIMUM) & pl.col("ic").is_finite())
        )
        for d_idx, ic_val in zip(agg["day_index"].to_list(), agg["ic"].to_list()):
            daily_ics[idx][int(d_idx)] = float(ic_val)
    if base.is_empty():
        return base.select(*meta_cols).with_columns(
            pl.lit(None, dtype=pl.Float64).alias("value")
        )
    sum_ic = [0.0] * k
    cnt_ic = [0] * k
    weight_rows = np.empty((len(days), k), dtype=float)
    for t in range(len(days)):
        realized = t - realized_lag
        if realized >= 0:
            for idx in range(k):
                ic_val = daily_ics[idx].get(realized)
                if ic_val is not None:
                    sum_ic[idx] += ic_val
                    cnt_ic[idx] += 1
        trailing = [sum_ic[idx] / cnt_ic[idx] if cnt_ic[idx] else 0.0 for idx in range(k)]
        denom = sum(abs(w) for w in trailing)
        weight_rows[t] = [w / denom for w in trailing] if denom > 0 else [1.0 / k] * k
    day_idx = base["day"].replace_strict(day_to_idx).to_numpy()
    w_mat = weight_rows[day_idx]
    v_mat = np.column_stack([
        base[f"v{idx}"].cast(pl.Float64).fill_null(np.nan).to_numpy() for idx in range(k)
    ])
    valid = np.isfinite(v_mat)
    num = np.sum(np.where(valid, w_mat * v_mat, 0.0), axis=1)
    den = np.sum(np.where(valid, np.abs(w_mat), 0.0), axis=1)
    combined = np.divide(num, den, out=np.full(len(num), np.nan), where=den > 0)
    combined_series = pl.Series("value", combined, dtype=pl.Float64)
    return base.select(*meta_cols).with_columns(
        pl.when(combined_series.is_finite()).then(combined_series).otherwise(None).alias("value")
    )


def evaluate(frame: pl.DataFrame, horizons: list, groups: int, holdout_day: str = "", *,
             return_mode: str = "", vol_adjust: bool = False, cost_bps: float = 0.0) -> dict:
    """IC at each horizon, the days' IC and group returns at the first, and
    each group's mean return and turnover; optionally split at `holdout_day`."""
    if return_mode not in ("", "close", "open"):
        raise ValueError(f"unknown factor return mode: {return_mode!r}")
    if not math.isfinite(cost_bps) or cost_bps < 0:
        raise ValueError("cost_bps must be a non-negative finite number")
    days = sorted(set(frame["day"]))
    split = None
    if holdout_day:
        split = bisect_left(days, holdout_day)
        if not 0 < split < len(days):
            raise ValueError("the holdout day leaves no days before it or after it")
    index = {day: i for i, day in enumerate(days)}
    frame = frame.sort("symbol", "day").with_columns(
        pl.col("day").replace_strict(index).alias("day_index"))
    all_horizons = sorted({1, *horizons})
    if return_mode == "open":
        if "open" not in frame.columns:
            raise ValueError("open-to-open return evaluation requires open prices")
        opens = frame.select("symbol", "day_index", pl.col("open").alias("ref_open"))
        frame = frame.join(
            opens.with_columns((pl.col("day_index") - 1).alias("day_index"))
            .rename({"ref_open": "entry_open"}),
            on=["symbol", "day_index"], how="left",
        )
        for h in all_horizons:
            frame = (
                frame.join(
                    opens.with_columns((pl.col("day_index") - h - 1).alias("day_index"))
                    .rename({"ref_open": "exit_open"}),
                    on=["symbol", "day_index"], how="left",
                )
                .with_columns(
                    pl.when((pl.col("entry_open") > 0) & pl.col("exit_open").is_finite())
                    .then(pl.col("exit_open") / pl.col("entry_open") - 1)
                    .otherwise(None)
                    .alias(f"ahead{h}")
                )
                .drop("exit_open")
            )
        frame = frame.drop("entry_open")
    else:
        future = frame.select("symbol", "day_index", pl.col("close").alias("future_close"))
        for h in all_horizons:
            frame = (frame.join(future.with_columns((pl.col("day_index") - h).alias("day_index")),
                                on=["symbol", "day_index"], how="left")
                     .with_columns((pl.col("future_close") / pl.col("close") - 1).alias(f"ahead{h}"))
                     .drop("future_close"))
    if vol_adjust:
        ret1 = (pl.col("close") / pl.col("close").shift(1) - 1).over("symbol")
        vol20 = (
            pl.when(ret1.is_finite()).then(ret1).otherwise(None)
            .rolling_std(window_size=20, min_samples=5)
            .over("symbol")
        )
        frame = frame.with_columns(
            pl.when(vol20.is_finite() & (vol20 > 0)).then(vol20).otherwise(None).alias("_vol20")
        )
        frame = frame.with_columns(pl.col("_vol20").median().over("day").alias("_med_vol"))
        frame = frame.with_columns([
            pl.when(pl.col("_vol20").is_not_null() & pl.col("_med_vol").is_not_null())
            .then(pl.col(f"ahead{h}") * (pl.col("_med_vol") / pl.col("_vol20")))
            .otherwise(None)
            .alias(f"ahead{h}")
            for h in all_horizons
        ]).drop("_vol20", "_med_vol")
    summary, dev_summary, holdout_summary, first_ic = [], [], [], {}
    holdout_lag = 1 if return_mode == "open" else 0
    for n, h in enumerate(horizons):
        ranked = frame.filter(pl.col("value").is_not_null() & pl.col(f"ahead{h}").is_finite())
        daily = (ranked.group_by("day", "day_index")
                 .agg(pl.len().alias("count"),
                      pl.corr("value", f"ahead{h}", method="spearman").alias("ic"))
                 .filter((pl.col("count") >= MINIMUM) & pl.col("ic").is_finite())
                 .sort("day"))
        ic = daily["ic"].to_numpy()
        if n == 0:
            first_ic = dict(zip(daily["day"], ic))
        summary.append(statistics(h, ic))
        if split is not None:
            dev_ic = daily.filter(pl.col("day_index") + h + holdout_lag < split)["ic"].to_numpy()
            out_ic = daily.filter(pl.col("day_index") >= split)["ic"].to_numpy()
            dev_summary.append(statistics(h, dev_ic))
            holdout_summary.append(statistics(h, out_ic))
    # Groups hold for one bar: compounding a longer horizon daily would count
    # its overlapping returns again and again.
    returns, members = group_returns(frame, 1, groups)
    # Cumulative from the first day with groups; a day without them keeps the last.
    total, long_short, net_long_short, state, history = [1.0] * groups, 1.0, 1.0, None, []
    prev_top, prev_bot = None, None
    cost_rate = cost_bps / 10000.0
    for day in days:
        if day in returns:
            total = [value * (1 + r) for value, r in zip(total, returns[day])]
            gross_r = returns[day][-1] - returns[day][0]
            long_short *= 1 + gross_r
            net_val = None
            if cost_bps > 0:
                top_set, bot_set = members[day][-1], members[day][0]
                if prev_top is None:
                    one_way = 2.0
                else:
                    t_top = len(top_set - prev_top) / len(top_set) if top_set else 0.0
                    t_bot = len(bot_set - prev_bot) / len(bot_set) if bot_set else 0.0
                    one_way = 2.0 * (t_top + t_bot)
                prev_top, prev_bot = top_set, bot_set
                net_long_short *= max(0.0, 1.0 + gross_r - one_way * cost_rate)
                net_val = net_long_short
            state = (total, long_short, net_val)
        history.append(state)
    counts = dict(frame.filter(pl.col("value").is_not_null()).group_by("day").len().iter_rows())
    return {
        "horizons": summary,
        "development_horizons": dev_summary,
        "holdout_horizons": holdout_summary,
        "days": [{"day": day, "symbols": counts.get(day, 0), "ic": first_ic.get(day),
                  "groups": state[0] if state else [], "long_short": state[1] if state else None,
                  "net_long_short": state[2] if state else None}
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
            # Returns over `horizon` bars overlap: about count / horizon are independent.
            result["t"] = result["ir"] * math.sqrt(count / horizon)
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


def run(directory: Path, admitted, reply):
    """Evaluates an AdmittedFactor into a FactorResult."""
    request = admitted.evaluation
    if not request.horizons or any(h < 1 for h in request.horizons) \
            or len(set(request.horizons)) != len(request.horizons):
        raise ValueError("horizons are distinct whole numbers of bars, at least one")
    if request.groups < 2:
        raise ValueError("a factor is split into at least two groups")
    if not request.symbols:
        raise ValueError("a factor is evaluated over one or more symbols")
    function, inputs, defaults = repository.factor(Path(admitted.source), request.factor)
    settings = settings_of(request, defaults)
    verify_factor_causality(function, inputs, settings, request.factor)
    extras = []
    for item in request.extra_factors:
        ex_fn, ex_inputs, ex_defaults = repository.factor(Path(admitted.source), item.factor)
        ex_settings = settings_of(item, ex_defaults)
        verify_factor_causality(ex_fn, ex_inputs, ex_settings, item.factor)
        extras.append((item.factor, ex_fn, ex_inputs, ex_settings))
    controls = []
    for item in request.orthogonalize_against:
        c_fn, c_inputs, c_defaults = repository.factor(Path(admitted.source), item.factor)
        c_settings = settings_of(item, c_defaults)
        verify_factor_causality(c_fn, c_inputs, c_settings, item.factor)
        controls.append((item.factor, c_fn, c_inputs, c_settings))
    data = Data(Dataset(directory), request.first_day, request.last_day,
                version=admitted.pinned.dataset_version)
    prepared = {}
    symbols, skipped = symbols_of(data, list(request.symbols), prepared)
    if not symbols:
        raise ValueError("no symbol has daily bars in the range")
    symbol_bars = {symbol: bars(data, symbol, prepared[symbol]) for symbol in symbols}
    primary = filter_liquidity(
        values(function, inputs, settings, symbol_bars),
        request.min_volume,
        request.min_open_interest,
    )
    control_frames = [
        filter_liquidity(values(fn, inp, cfg, symbol_bars),
                         request.min_volume, request.min_open_interest)
        for _, fn, inp, cfg in controls
    ]
    if extras:
        all_frames = [
            primary,
            *(filter_liquidity(values(fn, inp, cfg, symbol_bars),
                               request.min_volume, request.min_open_interest)
              for _, fn, inp, cfg in extras),
        ]
        if request.winsorize:
            all_frames = [_apply_daily(f, winsorize_mad) for f in all_frames]
        corr_matrix = factor_correlations(all_frames)
        combined = combine_factors(
            all_frames,
            request.combination,
            request.horizons[0],
            request.standardize,
            request.return_mode,
        )
        evaluated_frame = preprocess_frame(
            combined,
            neutralize=request.neutralize,
            controls=control_frames,
        )
        factor_names = [request.factor, *(name for name, _, _, _ in extras)]
    else:
        corr_matrix = []
        factor_names = []
        evaluated_frame = preprocess_frame(
            primary,
            winsorize=request.winsorize,
            standardize=request.standardize,
            neutralize=request.neutralize,
            controls=control_frames,
        )
    result = evaluate(
        evaluated_frame,
        list(request.horizons),
        request.groups,
        request.holdout_day,
        return_mode=request.return_mode,
        vol_adjust=request.vol_adjust,
        cost_bps=request.cost_bps,
    )
    reply.pinned.CopyFrom(admitted.pinned)
    reply.symbols.extend(symbols)
    for symbol, reason in skipped:
        reply.skipped.add(symbol=symbol, reason=reason)
    def fill_horizons(target, rows):
        for item in rows:
            entry = target.add(horizon=item["horizon"], days=item["days"])
            for name, field in (("mean", "mean"), ("deviation", "deviation"), ("ir", "ir"),
                                ("t", "t"), ("positive", "positive_share")):
                if item[name] is not None:
                    setattr(entry, field, item[name])
    fill_horizons(reply.horizons, result["horizons"])
    fill_horizons(reply.development_horizons, result["development_horizons"])
    fill_horizons(reply.holdout_horizons, result["holdout_horizons"])
    reply.factor_names.extend(factor_names)
    for correlation in corr_matrix:
        reply.correlations.add(**correlation)
    for item in result["days"]:
        entry = reply.days.add(trading_day=item["day"], symbols=item["symbols"],
                               groups=item["groups"])
        if item["ic"] is not None:
            entry.ic = item["ic"]
        if item["long_short"] is not None:
            entry.long_short = item["long_short"]
        if item["net_long_short"] is not None:
            entry.net_long_short = item["net_long_short"]
    for item in result["groups"]:
        entry = reply.groups.add(group=item["group"])
        for name in ("mean_return", "turnover"):
            if item[name] is not None:
                setattr(entry, name, item[name])


def main():
    from asterion.lab_pb2 import AdmittedFactor, FactorResult

    # What factor code prints goes to standard error; standard output carries the reply.
    output = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    request, reply = AdmittedFactor(), FactorResult()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request, reply)
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
    output.write(reply.SerializeToString())
    output.flush()
