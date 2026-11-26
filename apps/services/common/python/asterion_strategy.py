"""What a strategy is written against.

A strategy is a class in the user's own strategy repository. It reads completed
bars and says what it wants to hold from the next bar on; it never sends
orders. The engine turns what it wants into whole lots and orders, applies the
risk rules and keeps every amount in fixed point.

    class MaCross(Strategy):
        \"\"\"Long above the slow average, short below it.\"\"\"
        params = {"fast": 5, "slow": 20, "lots": 1}

        def ready(self, ctx):
            return all(len(ctx.history(s, "close", self.params["slow"])) == self.params["slow"]
                       for s in ctx.symbols)

        def on_bar(self, ctx, bar):
            closes = ctx.history(bar.symbol, "close", self.params["slow"])
            if len(closes) < self.params["slow"]:
                return
            up = closes[-self.params["fast"]:].mean() > closes.mean()
            ctx.target(bar.symbol, self.params["lots"] if up else -self.params["lots"])

A strategy uses a factor of the repository's factors/ by importing it:
ctx.factor(momentum, bar.symbol, 21, lookback=20) is its value at this bar.

A rule over several symbols overrides on_bars instead, which sees every bar
of one time together, and may hold weights rather than lots.

A product's dominant series is adjusted as of each bar: when it rolls into a
new month, the prices already in its history are scaled by new over old
settlement on the day before, and the new month keeps its own prices. Later
bars never change what an earlier bar saw. State a strategy keeps outside
ctx.history is not scaled; a change of bar.contract marks the roll.
"""
from dataclasses import dataclass
import datetime
from decimal import Decimal, ROUND_DOWN, ROUND_HALF_EVEN
import functools
import inspect
import math
import numbers

import numpy as np

FIELDS = (
    "open",
    "high",
    "low",
    "close",
    "volume",
    "open_interest",
    "roll_yield",
    "far_close",
    "near_return",
    "far_return",
    "receipt_volume",
    "member_long",
    "member_short",
    "inventory_total",
)
FIELDS_SET = frozenset(FIELDS)
PRICES = ("open", "high", "low", "close")
KINDS = {bool: "bool", int: "int", float: "float", str: "str"}
WEIGHT_STEP = Decimal("0.00000001")


@functools.lru_cache(maxsize=256)
def _factor_inputs(function) -> tuple[str, ...]:
    return tuple(name for name in inspect.signature(function).parameters if name in FIELDS_SET)


def quantize_weight(value: float) -> Decimal:
    # Convert via str() with explicit ROUND_HALF_EVEN so backtest, rolling and live agree.
    return Decimal(str(value)).quantize(WEIGHT_STEP, rounding=ROUND_HALF_EVEN)


def check_factor(function, inputs: dict, count: int, **settings):
    """Check every prefix of a test sample for causality and the chosen history window.

    `inputs` maps bar field names to one-dimensional arrays of equal length.
    `count` is the history length used by ctx.factor, including the current bar.
    Run on short samples, including warmup, missing values and parameter changes.
    """
    if type(count) is not int or count < 1:
        raise ValueError("factor history count must be a positive integer")
    arrays = {name: np.asarray(values, dtype=float).view() for name, values in inputs.items()}
    size = len(next(iter(arrays.values())))
    for values in arrays.values():
        if values.shape != (size,):
            raise ValueError("factor inputs must be one-dimensional arrays of equal length")
        values.flags.writeable = False

    def compute(begin, end):
        result = np.asarray(function(**{name: values[begin:end] for name, values in arrays.items()},
                                     **settings), dtype=float)
        if result.shape != (end - begin,):
            raise ValueError(f"factor must return one value per bar, got {result.shape}")
        return result

    full = compute(0, size)
    for end in range(1, size + 1):
        prefix = compute(0, end)
        if not np.allclose(prefix, full[:end], rtol=1e-10, atol=1e-12, equal_nan=True):
            raise ValueError(f"factor changes historical values when truncated to {end} bars")
        if end > count and not np.allclose(compute(end - count, end)[-1], prefix[-1],
                                          rtol=1e-10, atol=1e-12, equal_nan=True):
            raise ValueError(f"factor history window {count} differs from the full prefix at bar {end}")


def verify_factor_causality(function, inputs, settings: dict, name: str = ""):
    """Fast automatic lookahead check on a deterministic synthetic sample.

    Evaluates `function` on the full synthetic series and on truncated prefixes
    to catch future-dependent operations (negative shifts, full-series centering,
    or lookahead windows) before an evaluation runs.
    """
    if not inputs:
        return
    windows = [v for v in settings.values() if type(v) is int and 0 < v <= 260]
    span = max(windows, default=5)
    size = min(max(span + 8, 20), 280)
    rng = np.random.default_rng(20260401)
    returns = rng.normal(0.0005, 0.015, size)
    close = 100.0 * np.exp(np.cumsum(returns))
    opening = close * np.exp(rng.normal(0.0, 0.004, size))
    high = np.maximum(opening, close) * (1.0 + rng.uniform(0.001, 0.012, size))
    low = np.minimum(opening, close) * (1.0 - rng.uniform(0.001, 0.012, size))
    far_close = close * np.exp(rng.normal(0.0, 0.01, size))
    sample = {
        "open": opening,
        "high": high,
        "low": low,
        "close": close,
        "volume": rng.uniform(500.0, 5000.0, size),
        "open_interest": rng.uniform(2000.0, 20000.0, size),
        "roll_yield": rng.normal(0.0, 0.08, size),
        "far_close": far_close,
        "near_return": rng.normal(0.0, 0.015, size),
        "far_return": rng.normal(0.0, 0.015, size),
        "receipt_volume": rng.uniform(1000.0, 10000.0, size),
        "member_long": rng.uniform(10000.0, 50000.0, size),
        "member_short": rng.uniform(10000.0, 50000.0, size),
        "inventory_total": rng.uniform(50000.0, 200000.0, size),
    }
    arrays = {}
    for field in inputs:
        arr = sample[field].copy()
        arr.flags.writeable = False
        arrays[field] = arr
    full = np.asarray(function(**arrays, **settings), dtype=float)
    label = f"factor {name}" if name else "factor"
    if full.shape != (size,):
        raise ValueError(f"{label} must return one value per bar, got {full.shape}")
    cuts = sorted({
        c for c in (1, 2, max(1, span - 1), span, span + 1, size // 2, size - 3, size - 2, size - 1)
        if 1 <= c < size
    })
    for end in cuts:
        prefix = np.asarray(
            function(**{field: arr[:end] for field, arr in arrays.items()}, **settings),
            dtype=float,
        )
        if prefix.shape != (end,):
            raise ValueError(f"{label} failed causality check: returned {prefix.shape} for {end} bars")
        if not np.allclose(prefix, full[:end], rtol=1e-10, atol=1e-12, equal_nan=True):
            raise ValueError(
                f"{label} failed causality check: historical values change when truncated to {end} bars"
            )


# China futures product -> sector classification for cross-sectional neutralization.
PRODUCT_SECTORS: dict[str, str] = {
    # Black / ferrous & coal
    **{p: "black" for p in ("rb", "hc", "wr", "ss", "i", "j", "jm", "sf", "sm", "zc", "tc")},
    # Nonferrous & battery metals
    **{p: "nonferrous" for p in ("cu", "bc", "al", "ad", "ao", "zn", "pb", "ni", "sn", "si", "ps", "lc")},
    # Precious metals
    **{p: "precious" for p in ("au", "ag", "pt", "pd")},
    # Energy, chemicals, building materials & freight
    **{p: "energy_chem" for p in (
        "sc", "fu", "lu", "bu", "ru", "nr", "br", "ta", "px", "pf", "pr", "pl",
        "ma", "me", "eg", "eb", "bz", "l", "pp", "v", "pg", "fg", "sa", "sh", "ur", "sp", "op", "ec",
    )},
    # Oilseeds & vegetable oils
    **{p: "oilseeds" for p in ("a", "b", "m", "y", "p", "oi", "ro", "rm", "rs", "pk", "so")},
    # Grains, softs, livestock & timber
    **{p: "agriculture" for p in (
        "c", "cs", "sr", "cf", "cy", "ap", "cj", "jd", "lh", "rr", "jr",
        "ri", "er", "lr", "wh", "ws", "wt", "pm", "bb", "fb", "lg",
    )},
    # Equity index & treasury futures
    **{p: "financial" for p in ("if", "ih", "ic", "im", "ts", "tf", "t", "tl")},
}


def product_sector(symbol: str) -> str:
    """Sector label for `EXCHANGE/product[/YYYY-MM]` or bare product code, empty if unknown."""
    parts = symbol.strip().split("/")
    product = (parts[1] if len(parts) >= 2 else parts[0]).lower()
    return PRODUCT_SECTORS.get(product, "")


def ts_mean(values, window: int) -> np.ndarray:
    """Strictly causal trailing arithmetic mean over `window` bars."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1:] = np.where(valid, np.mean(safe, axis=1), np.nan)
    return out


def ts_std(values, window: int, ddof: int = 1) -> np.ndarray:
    """Strictly causal trailing standard deviation over `window` bars."""
    if type(window) is not int or type(ddof) is not int or ddof < 0 or window <= ddof:
        raise ValueError("window must be an integer greater than ddof")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1:] = np.where(valid, np.std(safe, axis=1, ddof=ddof), np.nan)
    return out


def ts_delta(values, periods: int = 1) -> np.ndarray:
    """Strictly causal difference `values[t] - values[t - periods]`."""
    if type(periods) is not int or periods < 1:
        raise ValueError("periods must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) > periods:
        diff = arr[periods:] - arr[:-periods]
        out[periods:] = np.where(np.isfinite(diff), diff, np.nan)
    return out


def ts_pct_change(values, periods: int = 1) -> np.ndarray:
    """Strictly causal relative change `values[t] / values[t - periods] - 1`."""
    if type(periods) is not int or periods < 1:
        raise ValueError("periods must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) > periods:
        cur, prev = arr[periods:], arr[:-periods]
        ok = np.isfinite(cur) & np.isfinite(prev) & (prev != 0.0)
        ratio = np.divide(cur, prev, out=np.full_like(cur, np.nan), where=ok)
        out[periods:] = np.where(ok, ratio - 1.0, np.nan)
    return out


def ts_rank(values, window: int) -> np.ndarray:
    """Strictly causal trailing percentile rank of the current bar in `[0, 1]`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    if window == 1:
        out[:] = np.where(valid, 0.5, np.nan)
        return out
    cur = win[:, -1:]
    less = np.sum(win < cur, axis=1)
    equal = np.sum(win == cur, axis=1)
    out[window - 1:] = np.where(valid, (less + 0.5 * (equal - 1.0)) / (window - 1.0), np.nan)
    return out


def ts_zscore(values, window: int) -> np.ndarray:
    """Strictly causal trailing z-score `(x - mean) / std` over `window` bars."""
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    mu = np.mean(safe, axis=1)
    sigma = np.std(safe, axis=1, ddof=1)
    z = np.divide(safe[:, -1] - mu, sigma, out=np.zeros(len(win), dtype=float), where=sigma > 0)
    out[window - 1:] = np.where(valid, z, np.nan)
    return out


def ts_corr(x, y, window: int) -> np.ndarray:
    """Strictly causal trailing Pearson correlation over `window` bars."""
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    a = np.asarray(x, dtype=float)
    b = np.asarray(y, dtype=float)
    if a.ndim != 1 or a.shape != b.shape:
        raise ValueError("ts_corr inputs must be 1D arrays of equal length")
    out = np.full(len(a), np.nan)
    if len(a) < window:
        return out
    wa = np.lib.stride_tricks.sliding_window_view(a, window)
    wb = np.lib.stride_tricks.sliding_window_view(b, window)
    valid = np.all(np.isfinite(wa), axis=1) & np.all(np.isfinite(wb), axis=1)
    sa = np.where(valid[:, None], wa, 0.0)
    sb = np.where(valid[:, None], wb, 0.0)
    da = sa - np.mean(sa, axis=1, keepdims=True)
    db = sb - np.mean(sb, axis=1, keepdims=True)
    denom = np.sqrt(np.sum(da * da, axis=1) * np.sum(db * db, axis=1))
    corr = np.divide(np.sum(da * db, axis=1), denom, out=np.full(len(wa), np.nan), where=denom > 0)
    out[window - 1:] = np.where(valid, np.clip(corr, -1.0, 1.0), np.nan)
    return out


def decay_linear(values, window: int) -> np.ndarray:
    """Strictly causal linearly weighted moving average with weights `1..window`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    weights = np.arange(1, window + 1, dtype=float)
    weights /= weights.sum()
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1:] = np.where(valid, safe @ weights, np.nan)
    return out


def _annualize_scale(annualize: float) -> float:
    if (
        not isinstance(annualize, numbers.Real)
        or isinstance(annualize, bool)
        or not math.isfinite(annualize)
        or annualize < 0
    ):
        raise ValueError("annualize must be a nonnegative finite number")
    return math.sqrt(float(annualize)) if annualize > 0 else 1.0


def _positive_log(values) -> np.ndarray:
    arr = np.asarray(values, dtype=float)
    ok = np.isfinite(arr) & (arr > 0.0)
    return np.where(ok, np.log(np.where(ok, arr, 1.0)), np.nan)


def ts_ewma_vol(close, span: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal EWMA volatility of log returns (`alpha = 2 / (span + 1)`)."""
    if type(span) is not int or span < 2:
        raise ValueError("span must be an integer of at least 2")
    scale = _annualize_scale(annualize)
    arr = np.asarray(close, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < 2:
        return out
    alpha = 2.0 / (span + 1.0)
    var = None
    seen = 0
    for i in range(1, len(arr)):
        prev, cur = arr[i - 1], arr[i]
        if not (math.isfinite(prev) and math.isfinite(cur) and prev > 0 and cur > 0):
            continue
        r = math.log(cur / prev)
        sq = r * r
        var = sq if var is None else (1.0 - alpha) * var + alpha * sq
        seen += 1
        if seen >= span:
            out[i] = math.sqrt(var) * scale
    return out


def ts_ema(values, span: int) -> np.ndarray:
    """Strictly causal exponential moving average (`alpha = 2 / (span + 1)`)."""
    if type(span) is not int or span < 1:
        raise ValueError("span must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    alpha = 2.0 / (span + 1.0)
    ema = None
    seen = 0
    for i, val in enumerate(arr):
        if not math.isfinite(val):
            continue
        ema = float(val) if ema is None else (1.0 - alpha) * ema + alpha * float(val)
        seen += 1
        if seen >= span:
            out[i] = ema
    return out


def _rolling_ols(y, x, window: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    ay = np.asarray(y, dtype=float)
    ax = np.asarray(x, dtype=float)
    if ay.ndim != 1 or ay.shape != ax.shape:
        raise ValueError("rolling OLS inputs must be 1D arrays of equal length")
    n = len(ay)
    beta_out = np.full(n, np.nan)
    resid_out = np.full(n, np.nan)
    r2_out = np.full(n, np.nan)
    if n < window:
        return beta_out, resid_out, r2_out
    wy = np.lib.stride_tricks.sliding_window_view(ay, window)
    wx = np.lib.stride_tricks.sliding_window_view(ax, window)
    valid = np.all(np.isfinite(wy), axis=1) & np.all(np.isfinite(wx), axis=1)
    sy = np.where(valid[:, None], wy, 0.0)
    sx = np.where(valid[:, None], wx, 0.0)
    my = np.mean(sy, axis=1)
    mx = np.mean(sx, axis=1)
    dy = sy - my[:, None]
    dx = sx - mx[:, None]
    sxx = np.sum(dx * dx, axis=1)
    syy = np.sum(dy * dy, axis=1)
    sxy = np.sum(dx * dy, axis=1)
    ok_x = valid & (sxx > 1e-24)
    beta = np.divide(sxy, sxx, out=np.full(len(wy), np.nan), where=ok_x)
    alpha = my - beta * mx
    resid = sy[:, -1] - (alpha + beta * sx[:, -1])
    ok_r2 = ok_x & (syy > 1e-24)
    r2 = np.divide(sxy * sxy, sxx * syy, out=np.full(len(wy), np.nan), where=ok_r2)
    beta_out[window - 1:] = np.where(ok_x, beta, np.nan)
    resid_out[window - 1:] = np.where(ok_x, resid, np.nan)
    r2_out[window - 1:] = np.where(ok_r2, np.clip(r2, 0.0, 1.0), np.nan)
    return beta_out, resid_out, r2_out


def ts_ols_beta(y, x, window: int) -> np.ndarray:
    """Strictly causal rolling OLS slope of `y = alpha + beta * x` over `window` bars."""
    return _rolling_ols(y, x, window)[0]


def ts_ols_resid(y, x, window: int) -> np.ndarray:
    """Strictly causal trailing OLS residual `y_t - (alpha + beta * x_t)` over `window` bars."""
    return _rolling_ols(y, x, window)[1]


def ts_ols_r2(y, x, window: int) -> np.ndarray:
    """Strictly causal rolling OLS R-squared of `y` on `x` over `window` bars."""
    return _rolling_ols(y, x, window)[2]


def kalman_filter(
    values,
    *,
    process_var: float = 1e-4,
    obs_var: float = 1e-2,
) -> np.ndarray:
    """Strictly causal 1D local-level Kalman filter.

    Reference:
        Kalman, R. E. (1960). A New Approach to Linear Filtering and Prediction
        Problems. Journal of Basic Engineering, 82(1), 35-45.
        https://doi.org/10.1115/1.3662552

    Equations:
        State: x_t = x_{t-1} + w_t,  w_t ~ N(0, Q)
        Obs:   z_t = x_t + v_t,      v_t ~ N(0, R)
        Predict: P_{t|t-1} = P_{t-1|t-1} + Q
        Update:  K_t = P_{t|t-1} / (P_{t|t-1} + R),
                 x_{t|t} = x_{t|t-1} + K_t (z_t - x_{t|t-1}),
                 P_{t|t} = (1 - K_t) P_{t|t-1}.

    Project adaptation:
        Initialized on the first finite observation (x_0 = z_0, P_0 = R);
        missing bars propagate prediction variance without overwriting state and emit NaN.
    """
    if (
        not isinstance(process_var, numbers.Real)
        or isinstance(process_var, bool)
        or not math.isfinite(process_var)
        or process_var < 0.0
    ):
        raise ValueError("process_var must be a nonnegative finite number")
    if (
        not isinstance(obs_var, numbers.Real)
        or isinstance(obs_var, bool)
        or not math.isfinite(obs_var)
        or obs_var <= 0.0
    ):
        raise ValueError("obs_var must be a positive finite number")
    q, r = float(process_var), float(obs_var)
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    x_hat = None
    p = r
    for i, z in enumerate(arr):
        if not math.isfinite(z):
            if x_hat is not None:
                p += q
            continue
        if x_hat is None:
            x_hat = float(z)
            p = r
        else:
            p_pred = p + q
            gain = p_pred / (p_pred + r)
            x_hat = x_hat + gain * (float(z) - x_hat)
            p = (1.0 - gain) * p_pred
        out[i] = x_hat
    return out


def kalman_hedge(
    y,
    x,
    *,
    delta: float = 1e-4,
    obs_var: float = 1e-3,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Strictly causal 2-state Kalman dynamic regression `y_t = beta_t * x_t + alpha_t + e_t`.

    References:
        Kalman, R. E. (1960). A New Approach to Linear Filtering and Prediction
        Problems. Journal of Basic Engineering, 82(1), 35-45.
        https://doi.org/10.1115/1.3662552
        Pole, A., West, M., & Harrison, J. (1994). Applied Bayesian Forecasting
        and Time Series Analysis. Chapman & Hall.

    Equations:
        theta_t = [beta_t, alpha_t]^T,  theta_t = theta_{t-1} + w_t,  Q = (delta / (1 - delta)) I_2
        H_t = [x_t, 1],  e_t = y_t - H_t theta_{t|t-1},  S_t = H_t P_{t|t-1} H_t^T + R
        K_t = P_{t|t-1} H_t^T / S_t,  theta_{t|t} = theta_{t|t-1} + K_t e_t,
        Joseph update: P_{t|t} = (I - K_t H_t) P_{t|t-1} (I - K_t H_t)^T + K_t R K_t^T.

    Project adaptation:
        Uses a diffuse prior P_0 = 1e6 * I_2 so arbitrary futures price levels
        converge within two bars; returns `(beta, intercept, spread)` where
        `spread[t]` is the strictly causal one-step-ahead innovation `e_t`
        (NaN on the first finite observation).
    """
    if (
        not isinstance(delta, numbers.Real)
        or isinstance(delta, bool)
        or not math.isfinite(delta)
        or not (0.0 < float(delta) < 1.0)
    ):
        raise ValueError("delta must be strictly in (0, 1)")
    if (
        not isinstance(obs_var, numbers.Real)
        or isinstance(obs_var, bool)
        or not math.isfinite(obs_var)
        or float(obs_var) <= 0.0
    ):
        raise ValueError("obs_var must be a positive finite number")
    ay = np.asarray(y, dtype=float)
    ax = np.asarray(x, dtype=float)
    if ay.ndim != 1 or ay.shape != ax.shape:
        raise ValueError("kalman_hedge inputs must be 1D arrays of equal length")
    n = len(ay)
    beta_out = np.full(n, np.nan)
    intercept_out = np.full(n, np.nan)
    spread_out = np.full(n, np.nan)
    q = float(delta) / (1.0 - float(delta))
    r = float(obs_var)
    t0, t1 = 0.0, 0.0
    p00, p01, p11 = 1e6, 0.0, 1e6
    seen = False
    for i in range(n):
        yt, xt = ay[i], ax[i]
        if not (math.isfinite(yt) and math.isfinite(xt)):
            if seen:
                p00 += q
                p11 += q
            continue
        p00_p = p00 + q if seen else p00
        p01_p = p01
        p11_p = p11 + q if seen else p11
        xf, yf = float(xt), float(yt)
        err = yf - (t0 * xf + t1)
        if seen:
            spread_out[i] = err
        s_var = xf * (p00_p * xf + p01_p) + (p01_p * xf + p11_p) + r
        k0 = (p00_p * xf + p01_p) / s_var
        k1 = (p01_p * xf + p11_p) / s_var
        t0 += k0 * err
        t1 += k1 * err
        a00, a01 = 1.0 - k0 * xf, -k0
        a10, a11 = -k1 * xf, 1.0 - k1
        b00 = a00 * p00_p + a01 * p01_p
        b01 = a00 * p01_p + a01 * p11_p
        b10 = a10 * p00_p + a11 * p01_p
        b11 = a10 * p01_p + a11 * p11_p
        p00 = b00 * a00 + b01 * a01 + k0 * r * k0
        p01 = 0.5 * ((b00 * a10 + b01 * a11) + (b10 * a00 + b11 * a01)) + k0 * r * k1
        p11 = b10 * a10 + b11 * a11 + k1 * r * k1
        seen = True
        beta_out[i] = t0
        intercept_out[i] = t1
    return beta_out, intercept_out, spread_out


def ts_autocorr(values, window: int, lag: int = 1) -> np.ndarray:
    """Strictly causal trailing Pearson autocorrelation at `lag` over `window` bars."""
    if type(window) is not int or type(lag) is not int or lag < 1 or window < lag + 2:
        raise ValueError("ts_autocorr requires lag >= 1 and window >= lag + 2")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    a, b = safe[:, :-lag], safe[:, lag:]
    da = a - np.mean(a, axis=1, keepdims=True)
    db = b - np.mean(b, axis=1, keepdims=True)
    denom = np.sqrt(np.sum(da * da, axis=1) * np.sum(db * db, axis=1))
    corr = np.divide(np.sum(da * db, axis=1), denom, out=np.full(len(win), np.nan), where=denom > 1e-24)
    out[window - 1:] = np.where(valid, np.clip(corr, -1.0, 1.0), np.nan)
    return out


def ts_skew(values, window: int) -> np.ndarray:
    """Strictly causal trailing skewness (standardized 3rd central moment) over `window` bars."""
    if type(window) is not int or window < 3:
        raise ValueError("ts_skew requires window >= 3")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    m2 = np.mean(centered * centered, axis=1)
    m3 = np.mean(centered ** 3, axis=1)
    ok = valid & (m2 > 1e-24)
    skew = np.divide(m3, np.power(np.where(ok, m2, 1.0), 1.5), out=np.full(len(win), np.nan), where=ok)
    out[window - 1:] = np.where(ok, skew, np.nan)
    return out


def ts_kurt(values, window: int) -> np.ndarray:
    """Strictly causal trailing excess kurtosis (`m4 / m2^2 - 3`) over `window` bars."""
    if type(window) is not int or window < 4:
        raise ValueError("ts_kurt requires window >= 4")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    m2 = np.mean(centered * centered, axis=1)
    m4 = np.mean(centered ** 4, axis=1)
    ok = valid & (m2 > 1e-24)
    kurt = np.divide(m4, np.where(ok, m2 * m2, 1.0), out=np.full(len(win), np.nan), where=ok) - 3.0
    out[window - 1:] = np.where(ok, kurt, np.nan)
    return out


def ts_variance_ratio(close, window: int, q: int = 5) -> np.ndarray:
    """Strictly causal Lo-MacKinlay overlapping log-return variance ratio `VR(q)`.

    Reference:
        Lo, A. W., & MacKinlay, A. C. (1988). Stock Market Prices Do Not Follow
        Random Walks: Evidence from a Simple Specification Test. Review of
        Financial Studies, 1(1), 41-66, Eqs. (12)-(13).
        https://doi.org/10.1093/rfs/1.1.41

    Equations:
        sigma_a^2 = sum (r_t - mu)^2 / (n - 1),
        sigma_c^2(q) = sum (sum_{j=0}^{q-1} (r_{t-j} - mu))^2 / (q * (n - q + 1) * (1 - q / n)),
        VR(q) = sigma_c^2(q) / sigma_a^2.

    Project adaptation:
        Operates on rolling windows of `window` log returns (requires `window + 1`
        positive closes); returns NaN when single-period variance is zero.
    """
    if type(window) is not int or type(q) is not int or not (2 <= q < window):
        raise ValueError("ts_variance_ratio requires 2 <= q < window")
    log_p = _positive_log(close)
    out = np.full(len(log_p), np.nan)
    if len(log_p) <= window:
        return out
    rets = np.diff(log_p)
    win = np.lib.stride_tricks.sliding_window_view(rets, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    var1 = np.sum(centered * centered, axis=1) / float(window - 1)
    sums = np.lib.stride_tricks.sliding_window_view(centered, q, axis=1).sum(axis=2)
    denom_q = float(q) * float(window - q + 1) * (1.0 - float(q) / float(window))
    var_q = np.sum(sums * sums, axis=1) / denom_q
    ok = valid & (var1 > 1e-24)
    vr = np.divide(var_q, var1, out=np.full(len(win), np.nan), where=ok)
    out[window:] = np.where(ok, vr, np.nan)
    return out


def ts_hurst(close, window: int = 100, max_lag: int = 20) -> np.ndarray:
    """Strictly causal rolling generalized Hurst exponent (q=2 structure function).

    References:
        Hurst, H. E. (1951). Long-Term Storage Capacity of Reservoirs.
        Transactions of the ASCE, 116, 770-799.
        https://doi.org/10.1061/TACEAT.0006518
        Di Matteo, T. (2007). Multi-scaling in finance. Quantitative Finance,
        7(1), 21-36, Eq. (2). https://doi.org/10.1080/14697680600969727

    Equations:
        K_2(tau) = mean((log P_{t+tau} - log P_t)^2) ~ tau^{2 H},
        H = slope of 0.5 * log K_2(tau) regressed on log(tau) for tau = 2..L.

    Project adaptation:
        Finite rolling window on log prices with L = min(max_lag, window // 2);
        values < 0.5 indicate mean reversion, ~0.5 random walk, > 0.5 persistence.
    """
    if type(window) is not int or type(max_lag) is not int or window < 8 or max_lag < 3:
        raise ValueError("ts_hurst requires window >= 8 and max_lag >= 3")
    log_p = _positive_log(close)
    out = np.full(len(log_p), np.nan)
    if len(log_p) < window:
        return out
    upper = min(max_lag, window // 2)
    if upper < 3:
        raise ValueError("ts_hurst requires min(max_lag, window // 2) >= 3")
    win = np.lib.stride_tricks.sliding_window_view(log_p, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    lags = np.arange(2, upper + 1, dtype=float)
    log_lags = np.log(lags)
    x_c = log_lags - np.mean(log_lags)
    sxx = float(np.sum(x_c * x_c))
    log_rms = np.empty((len(win), len(lags)), dtype=float)
    ok = valid.copy()
    for col, tau in enumerate(range(2, upper + 1)):
        diffs = safe[:, tau:] - safe[:, :-tau]
        msq = np.mean(diffs * diffs, axis=1)
        ok &= msq > 1e-24
        log_rms[:, col] = 0.5 * np.log(np.where(msq > 1e-24, msq, 1.0))
    y_c = log_rms - np.mean(log_rms, axis=1, keepdims=True)
    slope = (y_c @ x_c) / sxx
    out[window - 1:] = np.where(ok, slope, np.nan)
    return out


def ou_half_life(series, window: int) -> np.ndarray:
    """Strictly causal Ornstein-Uhlenbeck mean-reversion half-life (in bars).

    Reference:
        Uhlenbeck, G. E., & Ornstein, L. S. (1930). On the Theory of the Brownian
        Motion. Physical Review, 36(5), 823-841.
        https://doi.org/10.1103/PhysRev.36.823

    Equations:
        dS_t = theta (mu - S_t) dt + sigma dW_t,
        Discrete AR(1): delta S_t = alpha + beta * S_{t-1} + eps_t,  beta = exp(-theta) - 1,
        t_{1/2} = -ln(2) / ln(1 + beta)  for -1 < beta < 0.

    Project adaptation:
        Estimated causally via rolling OLS over `window` differences (requires
        `window + 1` points); returns NaN when beta >= 0 (non-reverting) or
        beta <= -1 (oscillatory overshoot).
    """
    if type(window) is not int or window < 3:
        raise ValueError("ou_half_life requires window >= 3")
    arr = np.asarray(series, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) <= window:
        return out
    beta = _rolling_ols(np.diff(arr), arr[:-1], window)[0]
    ok = np.isfinite(beta) & (beta < 0.0) & (beta > -1.0)
    hl = np.full_like(beta, np.nan)
    hl[ok] = -math.log(2.0) / np.log1p(beta[ok])
    out[1:] = hl
    return out


def parkinson_vol(high, low, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Parkinson high-low range volatility.

    Reference:
        Parkinson, M. (1980). The Extreme Value Method for Estimating the
        Variance of the Rate of Return. The Journal of Business, 53(1), 61-65.
        https://doi.org/10.1086/296071

    Equation:
        sigma_P = sqrt(mean(ln(H_t / L_t)^2) / (4 ln 2)) * sqrt(annualize).
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    scale = _annualize_scale(annualize)
    lh, ll = _positive_log(high), _positive_log(low)
    if lh.ndim != 1 or lh.shape != ll.shape:
        raise ValueError("parkinson_vol inputs must be 1D arrays of equal length")
    var = ts_mean(((lh - ll) ** 2) / (4.0 * math.log(2.0)), window)
    return np.where(np.isfinite(var), np.sqrt(np.maximum(var, 0.0)) * scale, np.nan)


def garman_klass_vol(open, high, low, close, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Garman-Klass OHLC volatility.

    Reference:
        Garman, M. B., & Klass, M. J. (1980). On the Estimation of Security
        Price Volatilities from Historical Data. The Journal of Business,
        53(1), 67-78. https://doi.org/10.1086/296072

    Equations:
        v_t = 0.5 * ln(H_t / L_t)^2 - (2 ln 2 - 1) * ln(C_t / O_t)^2,
        sigma_GK = sqrt(max(mean(v_t), 0)) * sqrt(annualize).
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    scale = _annualize_scale(annualize)
    lo, lh, ll, lc = _positive_log(open), _positive_log(high), _positive_log(low), _positive_log(close)
    if lo.ndim != 1 or not (lo.shape == lh.shape == ll.shape == lc.shape):
        raise ValueError("garman_klass_vol inputs must be 1D arrays of equal length")
    inst = 0.5 * (lh - ll) ** 2 - (2.0 * math.log(2.0) - 1.0) * (lc - lo) ** 2
    var = ts_mean(inst, window)
    return np.where(np.isfinite(var), np.sqrt(np.maximum(var, 0.0)) * scale, np.nan)


def yang_zhang_vol(open, high, low, close, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Yang-Zhang drift- and gap-independent OHLC volatility.

    Reference:
        Yang, D., & Zhang, Q. (2000). Drift-Independent Volatility Estimation
        Based on High, Low, Open, and Close Prices. The Journal of Business,
        73(3), 477-492, Eqs. (7), (18). https://doi.org/10.1086/209650

    Equations:
        sigma_YZ^2 = sigma_o^2 + k * sigma_c^2 + (1 - k) * sigma_RS^2,
        k = 0.34 / (1.34 + (n + 1) / (n - 1)).
    """
    if type(window) is not int or window < 2:
        raise ValueError("yang_zhang_vol requires window >= 2")
    scale = _annualize_scale(annualize)
    lo, lh, ll, lc = _positive_log(open), _positive_log(high), _positive_log(low), _positive_log(close)
    if lo.ndim != 1 or not (lo.shape == lh.shape == ll.shape == lc.shape):
        raise ValueError("yang_zhang_vol inputs must be 1D arrays of equal length")
    prev_c = np.full(len(lc), np.nan)
    prev_c[1:] = lc[:-1]
    overnight = ts_std(lo - prev_c, window, ddof=1) ** 2
    intraday = ts_std(lc - lo, window, ddof=1) ** 2
    rs = ts_mean((lh - lo) * (lh - lc) + (ll - lo) * (ll - lc), window)
    k = 0.34 / (1.34 + float(window + 1) / float(window - 1))
    total = overnight + k * intraday + (1.0 - k) * rs
    return np.where(np.isfinite(total), np.sqrt(np.maximum(total, 0.0)) * scale, np.nan)


def corwin_schultz_spread(high, low, window: int = 1) -> np.ndarray:
    """Strictly causal Corwin-Schultz high-low implied bid-ask spread estimator.

    Reference:
        Corwin, S. A., & Schultz, P. (2012). A Simple Way to Estimate Bid-Ask
        Spreads from Daily High and Low Prices. The Journal of Finance, 67(2),
        719-760, Eqs. (14)-(18). https://doi.org/10.1111/j.1540-6261.2012.01729.x

    Equations:
        beta_t = ln(H_t / L_t)^2 + ln(H_{t-1} / L_{t-1})^2,
        gamma_t = ln(max(H_t, H_{t-1}) / min(L_t, L_{t-1}))^2,
        alpha_t = max(0, (sqrt(2 beta_t) - sqrt(beta_t)) / (3 - 2 sqrt(2))
                         - sqrt(gamma_t / (3 - 2 sqrt(2)))),
        S_t = 2 (exp(alpha_t) - 1) / (1 + exp(alpha_t)).

    Project adaptation:
        Supports both daily and minute OHLC bars; when `window > 1`, returns the
        causal rolling mean of adjacent-bar spread estimates over `window` bars.
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    ah = np.asarray(high, dtype=float)
    al = np.asarray(low, dtype=float)
    if ah.ndim != 1 or ah.shape != al.shape:
        raise ValueError("corwin_schultz_spread inputs must be 1D arrays of equal length")
    n = len(ah)
    raw_spread = np.full(n, np.nan)
    if n >= 2:
        h0, h1 = ah[:-1], ah[1:]
        l0, l1 = al[:-1], al[1:]
        ok = (
            np.isfinite(h0) & np.isfinite(h1) & np.isfinite(l0) & np.isfinite(l1)
            & (h0 > 0.0) & (h1 > 0.0) & (l0 > 0.0) & (l1 > 0.0)
            & (h0 >= l0) & (h1 >= l1)
        )
        sh0, sh1 = np.where(ok, h0, 1.0), np.where(ok, h1, 1.0)
        sl0, sl1 = np.where(ok, l0, 1.0), np.where(ok, l1, 1.0)
        beta = np.log(sh0 / sl0) ** 2 + np.log(sh1 / sl1) ** 2
        gamma = np.log(np.maximum(sh0, sh1) / np.minimum(sl0, sl1)) ** 2
        denom = 3.0 - 2.0 * math.sqrt(2.0)
        alpha = np.maximum(0.0, (np.sqrt(2.0 * beta) - np.sqrt(beta)) / denom - np.sqrt(gamma / denom))
        exp_a = np.exp(np.minimum(alpha, 50.0))
        s = 2.0 * (exp_a - 1.0) / (1.0 + exp_a)
        raw_spread[1:] = np.where(ok, s, np.nan)
    return raw_spread if window == 1 else ts_mean(raw_spread, window)


def ts_vwap_dev(close, volume, window: int) -> np.ndarray:
    """Strictly causal deviation from trailing volume-weighted average price `close / VWAP - 1`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    ac = np.asarray(close, dtype=float)
    av = np.asarray(volume, dtype=float)
    if ac.ndim != 1 or ac.shape != av.shape:
        raise ValueError("ts_vwap_dev inputs must be 1D arrays of equal length")
    out = np.full(len(ac), np.nan)
    if len(ac) < window:
        return out
    wc = np.lib.stride_tricks.sliding_window_view(ac, window)
    wv = np.lib.stride_tricks.sliding_window_view(av, window)
    valid = np.all(np.isfinite(wc), axis=1) & np.all(np.isfinite(wv), axis=1) & np.all(wv >= 0.0, axis=1)
    sc = np.where(valid[:, None], wc, 0.0)
    sv = np.where(valid[:, None], wv, 0.0)
    vol_sum = np.sum(sv, axis=1)
    pv_sum = np.sum(sc * sv, axis=1)
    ok = valid & (vol_sum > 0.0) & (pv_sum > 0.0)
    vwap = np.divide(pv_sum, vol_sum, out=np.full(len(wc), np.nan), where=ok)
    out[window - 1:] = np.where(ok, sc[:, -1] / np.where(ok, vwap, 1.0) - 1.0, np.nan)
    return out


@dataclass(frozen=True)
class Bar:
    """One completed bar. Prices are read as floats for research; the ledger
    never sees them."""
    symbol: str  # what the run trades: a contract, or a product's dominant series
    contract: str  # the contract the bar is of; a series' month of that day
    time: datetime.datetime  # when the bar completed, Beijing time
    open: float
    high: float
    low: float
    close: float
    volume: float
    open_interest: float
    roll_yield: float = math.nan
    far_close: float = math.nan
    near_return: float = math.nan
    far_return: float = math.nan
    receipt_volume: float = math.nan
    member_long: float = math.nan
    member_short: float = math.nan
    inventory_total: float = math.nan


class Strategy:
    """The base of every strategy. `params` names each parameter with its
    default; a run may set any of them to a value of the same kind."""
    params: dict = {}
    history_limit: int | None = None

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        for name, value in cls.params.items():
            if not isinstance(name, str) or not name.isidentifier():
                raise TypeError(f"{cls.__qualname__}: parameter name {name!r} is not an identifier")
            if type(value) not in KINDS:
                raise TypeError(f"{cls.__qualname__}: parameter {name} defaults to a "
                                f"{type(value).__name__}; use bool, int, float or str")
        if cls.history_limit is not None:
            if (
                not isinstance(cls.history_limit, numbers.Integral)
                or isinstance(cls.history_limit, bool)
                or cls.history_limit < 1
            ):
                raise TypeError(f"{cls.__qualname__}: history_limit must be None or a positive int")
        if cls.on_bar is Strategy.on_bar and cls.on_bars is Strategy.on_bars:
            raise TypeError(f"{cls.__qualname__} defines neither on_bar nor on_bars")

    def __init__(self, **values):
        defaults = type(self).params
        unknown = sorted(set(values) - set(defaults))
        if unknown:
            raise ValueError(f"unknown parameters: {', '.join(unknown)}")
        params = dict(defaults)
        for name, value in values.items():
            kind = type(defaults[name])
            # A whole number is a float parameter's value too; nothing else converts.
            if kind is float and type(value) is int:
                value = float(value)
            if type(value) is not kind:
                raise ValueError(f"parameter {name} is a {KINDS[kind]}, not {value!r}")
            if kind is float and not math.isfinite(value):
                raise ValueError(f"parameter {name} is not finite")
            params[name] = value
        self.params = params

    def on_bars(self, ctx: "Context", bars: dict):
        """Every symbol's bar that completed at ctx.time, by symbol. A symbol
        without one is absent, never filled in."""
        for bar in bars.values():
            self.on_bar(ctx, bar)

    def on_bar(self, ctx: "Context", bar: Bar):
        """One completed bar."""

    def ready(self, ctx: "Context") -> bool:
        """Report readiness after on_bars, from actual parameters and state.

        Until explicitly ready, proposed targets are discarded in both research
        and live runs. Trading always starts with an empty context.
        """
        return False


class Column:
    """A growing or bounded array whose filled part is read without copying.

    Stays unallocated while only NaN values arrive, avoiding memory overhead for
    unused optional fields. When `max_len` is set, compacts at `2 * max_len` so
    memory stays O(max_len) while preserving O(1) amortized append and zero-copy views.
    """

    def __init__(self, max_len: int | None = None):
        self.max_len = None if max_len is None else int(max_len)
        self.values: np.ndarray | None = None
        self.size = 0

    @property
    def nbytes(self) -> int:
        return 0 if self.values is None else int(self.values.nbytes)

    def _allocate(self, min_size: int):
        if self.max_len is None:
            cap = 256
            while cap < min_size:
                cap *= 2
        else:
            limit = max(2, 2 * self.max_len)
            cap = min(256, limit)
            while cap < min_size and cap < limit:
                cap = min(limit, cap * 2)
        arr = np.empty(cap)
        if self.size:
            arr[:self.size] = math.nan
        self.values = arr

    def append(self, value: float):
        if self.values is None:
            if math.isnan(value):
                if self.max_len is None or self.size < self.max_len:
                    self.size += 1
                return
            self._allocate(self.size + 1)
        if self.size == len(self.values):
            if self.max_len is not None:
                limit = max(2, 2 * self.max_len)
                if len(self.values) < limit:
                    grown = np.empty(min(limit, len(self.values) * 2))
                    grown[:self.size] = self.values
                    self.values = grown
                else:
                    # A new array: views handed out before keep what they showed.
                    compacted = np.empty(len(self.values))
                    keep = self.max_len
                    compacted[:keep] = self.values[self.size - keep:self.size]
                    self.values = compacted
                    self.size = keep
            else:
                grown = np.empty(len(self.values) * 2)
                grown[:self.size] = self.values
                self.values = grown
        self.values[self.size] = value
        self.size += 1

    def scale(self, ratio: float):
        if self.values is None:
            return
        # A new array: views handed out before keep what they showed.
        scaled = np.empty_like(self.values)
        if self.max_len is not None and self.size > self.max_len:
            keep = self.max_len
            scaled[:keep] = self.values[self.size - keep:self.size] * ratio
            self.size = keep
        else:
            scaled[:self.size] = self.values[:self.size] * ratio
        self.values = scaled

    def last(self, count: int) -> np.ndarray:
        if self.max_len is not None and count > self.max_len:
            raise ValueError(f"requested {count} bars exceeds history_limit {self.max_len}")
        if self.values is None:
            view = np.full(min(count, self.size), math.nan)
            view.flags.writeable = False
            return view
        view = self.values[max(0, self.size - count):self.size]
        view.flags.writeable = False
        return view


class Context:
    """A run's view for its strategy: the bars so far and what it wants to hold.
    It offers no way to send an order."""

    def __init__(self, symbols, rolls=None, max_history: int | None = None):
        """`rolls`: for each month a dominant series rolls into, new over old
        settlement on the day before the roll. `max_history`: maximum bars
        retained per field, or None for full prefix."""
        self.symbols = tuple(symbols)
        if not self.symbols or len(set(self.symbols)) != len(self.symbols):
            raise ValueError("a run trades one or more distinct symbols")
        if max_history is not None:
            if (
                not isinstance(max_history, numbers.Integral)
                or isinstance(max_history, bool)
                or max_history < 1
            ):
                raise ValueError("max_history must be None or a positive int")
            max_history = int(max_history)
        self.max_history = max_history
        self.time = None
        self.columns = {
            symbol: {field: Column(max_history) for field in FIELDS}
            for symbol in self.symbols
        }
        self.rolls = dict(rolls or {})
        self.contracts = {}
        self.mode = None  # "target" or "weights", whichever the strategy used first
        self.wanted = {}
        self.ready = False

    def history(self, symbol: str, field: str, count: int) -> np.ndarray:
        """The last `count` values of a field, through the bar being decided on;
        fewer while there are not that many. Read-only."""
        if symbol not in self.columns:
            raise KeyError(f"{symbol} is not traded by this run")
        if field not in FIELDS_SET:
            raise KeyError(f"no bar field {field}")
        if not isinstance(count, numbers.Integral) or isinstance(count, bool) or count < 1:
            raise ValueError("count is a positive whole number")
        return self.columns[symbol][field].last(int(count))

    def factor(self, function, symbol: str, count: int, **settings) -> float:
        """A factor's value at the bar being decided on, computed over the
        symbol's last `count` bars: the function's parameters named after bar
        fields get that history, `settings` the rest."""
        inputs = {name: self.history(symbol, name, count) for name in _factor_inputs(function)}
        return float(function(**inputs, **settings)[-1])

    def target(self, symbol: str, lots: int):
        """Hold `lots` of the symbol from the next bar on: positive long,
        negative short, zero flat."""
        self.use("target")
        if symbol not in self.columns:
            raise KeyError(f"{symbol} is not traded by this run")
        if not isinstance(lots, numbers.Integral) or isinstance(lots, bool):
            raise TypeError(f"lots are whole: {lots!r}")
        self.wanted[symbol] = int(lots)

    def weights(self, weights: dict):
        """Hold these shares of equity from the next bar on: positive long,
        negative short. Symbols left out are held flat. The engine converts
        them into whole lots toward zero and leaves what remains unallocated."""
        self.use("weights")
        wanted = {}
        for symbol, weight in weights.items():
            if symbol not in self.columns:
                raise KeyError(f"{symbol} is not traded by this run")
            if not isinstance(weight, numbers.Real) or isinstance(weight, bool) \
                    or not math.isfinite(weight):
                raise TypeError(f"weight of {symbol} is not a finite number: {weight!r}")
            wanted[symbol] = float(weight)
        self.wanted = {symbol: wanted.get(symbol, 0.0) for symbol in self.symbols}

    def use(self, mode: str):
        if self.mode not in (None, mode):
            raise RuntimeError(f"this strategy already holds by {self.mode}; it cannot mix in {mode}")
        self.mode = mode


def observe(context: Context, bars: list) -> dict:
    """Add one time's completed bars to the context, adjusting a series that
    rolled. Returns them by symbol."""
    if not bars:
        raise ValueError("a step has at least one bar")
    time = bars[0].time
    by_symbol = {}
    for bar in bars:
        if bar.time != time:
            raise ValueError("a step's bars completed at one time")
        if bar.symbol not in context.columns or bar.symbol in by_symbol:
            raise ValueError(f"unexpected bar of {bar.symbol}")
        by_symbol[bar.symbol] = bar
    if context.time is not None and time <= context.time:
        raise ValueError(f"bars at {time} do not follow {context.time}")
    context.time = time
    for symbol, bar in by_symbol.items():
        columns = context.columns[symbol]
        if context.contracts.get(symbol, bar.contract) != bar.contract:
            for field in PRICES:
                columns[field].scale(context.rolls[bar.contract])
        context.contracts[symbol] = bar.contract
        for field in FIELDS:
            columns[field].append(getattr(bar, field))
    return by_symbol


def step(strategy: Strategy, context: Context, bars: list) -> dict:
    """Give the strategy one time's completed bars. Returns what it asked for:
    lots or weights by symbol, empty when it asked for nothing new."""
    by_symbol = observe(context, bars)
    context.wanted = {}
    strategy.on_bars(context, by_symbol)
    context.ready = strategy.ready(context)
    if type(context.ready) is not bool:
        raise TypeError("strategy readiness must be a bool")
    if not context.ready:
        context.wanted = {}
    return context.wanted


def cross_section_zscore(scores: dict, clip: float = 3.0) -> dict:
    """Cross-sectional z-score across finite values in `scores`, clipped to [-clip, clip]."""
    valid = [(symbol, float(val)) for symbol, val in scores.items()
             if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)]
    if len(valid) < 2:
        return {symbol: 0.0 for symbol, _ in valid}
    arr = np.array([val for _, val in valid], dtype=float)
    std = float(np.std(arr, ddof=1))
    if std <= 0 or not math.isfinite(std):
        return {symbol: 0.0 for symbol, _ in valid}
    z = (arr - float(np.mean(arr))) / std
    if clip > 0:
        z = np.clip(z, -float(clip), float(clip))
    return {symbol: float(z[i]) for i, (symbol, _) in enumerate(valid)}


def cross_section_rank(scores: dict) -> dict:
    """Cross-sectional centered rank in [-1.0, 1.0] across finite values (ties averaged)."""
    valid = [(symbol, float(val)) for symbol, val in scores.items()
             if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)]
    n = len(valid)
    if n == 0:
        return {}
    if n == 1:
        return {valid[0][0]: 0.0}
    arr = np.array([val for _, val in valid], dtype=float)
    order = np.argsort(arr, kind="mergesort")
    sorted_vals = arr[order]
    ranks = np.empty(n, dtype=float)
    i = 0
    while i < n:
        j = i + 1
        while j < n and sorted_vals[j] == sorted_vals[i]:
            j += 1
        avg_rank = 0.5 * (i + j - 1)
        ranks[order[i:j]] = avg_rank
        i = j
    scaled = 2.0 * ranks / (n - 1) - 1.0
    return {symbol: float(scaled[idx]) for idx, (symbol, _) in enumerate(valid)}


def winsorize_mad(scores: dict, n: float = 5.0) -> dict:
    """Clip cross-sectional outliers outside `median +- n * 1.4826 * MAD`."""
    if not isinstance(n, numbers.Real) or isinstance(n, bool) or not math.isfinite(n) or n <= 0:
        raise ValueError("n must be a positive finite number")
    valid = [(symbol, float(val)) for symbol, val in scores.items()
             if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)]
    if len(valid) < 2:
        return dict(valid)
    arr = np.array([val for _, val in valid], dtype=float)
    med = float(np.median(arr))
    mad = float(np.median(np.abs(arr - med)))
    if mad <= 0.0 or not math.isfinite(mad):
        return dict(valid)
    bound = float(n) * 1.4826 * mad
    clipped = np.clip(arr, med - bound, med + bound)
    return {symbol: float(clipped[i]) for i, (symbol, _) in enumerate(valid)}


def sector_neutralize(scores: dict, sectors: dict | None = None) -> dict:
    """Demean cross-sectional scores within each futures sector."""
    valid = [(symbol, float(val)) for symbol, val in scores.items()
             if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)]
    groups: dict[str, list[float]] = {}
    labels: dict[str, str] = {}
    for symbol, val in valid:
        sector = sectors[symbol] if sectors and symbol in sectors else product_sector(symbol)
        labels[symbol] = sector
        groups.setdefault(sector, []).append(val)
    means = {sector: float(np.mean(vals)) for sector, vals in groups.items()}
    return {symbol: val - means[labels[symbol]] for symbol, val in valid}


def orthogonalize(target: dict, controls) -> dict:
    """Cross-sectional OLS residual of `target` regressed on one or more control score dicts."""
    control_list = [controls] if isinstance(controls, dict) else list(controls)
    valid_target = {
        symbol: float(val) for symbol, val in target.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    }
    if not control_list:
        return valid_target
    common = [
        symbol for symbol in valid_target
        if all(
            symbol in ctrl
            and isinstance(ctrl[symbol], numbers.Real)
            and not isinstance(ctrl[symbol], bool)
            and math.isfinite(ctrl[symbol])
            for ctrl in control_list
        )
    ]
    if len(common) < len(control_list) + 2:
        return {}
    y = np.array([valid_target[symbol] for symbol in common], dtype=float)
    x = np.column_stack([
        np.ones(len(common), dtype=float),
        *([float(ctrl[symbol]) for symbol in common] for ctrl in control_list),
    ])
    beta, *_ = np.linalg.lstsq(x, y, rcond=None)
    resid = y - x @ beta
    return {symbol: float(resid[i]) for i, symbol in enumerate(common)}


def liquid_symbols(ctx: Context, bars: dict, min_bars: int = 1,
                   min_volume: float = 0.0, min_open_interest: float = 0.0,
                   window: int = 20) -> list:
    """Symbols trading at `ctx.time` with sufficient history, volume and open interest."""
    if min_bars < 1 or window < 1:
        raise ValueError("min_bars and window must be at least 1")
    eligible = []
    for symbol in ctx.symbols:
        bar = bars.get(symbol)
        if bar is None or not math.isfinite(bar.close) or bar.close <= 0:
            continue
        closes = ctx.history(symbol, "close", min_bars)
        if len(closes) < min_bars or not np.all(np.isfinite(closes)):
            continue
        if min_volume > 0:
            vols = ctx.history(symbol, "volume", window)
            finite_vols = vols[np.isfinite(vols)]
            if len(finite_vols) == 0 or float(np.mean(finite_vols)) < min_volume:
                continue
        if min_open_interest > 0:
            ois = ctx.history(symbol, "open_interest", window)
            finite_ois = ois[np.isfinite(ois)]
            if len(finite_ois) == 0 or float(np.mean(finite_ois)) < min_open_interest:
                continue
        eligible.append(symbol)
    return eligible


def vol_target_weights(
    signals: dict,
    volatilities: dict,
    target_vol: float = 0.10,
    gross_weight: float = 1.0,
    max_weight: float = 0.25,
) -> dict:
    """Scale finite signals by `target_vol / vol`, clip to `max_weight`, and cap gross exposure."""
    if target_vol <= 0 or gross_weight <= 0 or max_weight <= 0:
        raise ValueError("target_vol, gross_weight, and max_weight must be positive")
    raw = {}
    for symbol, signal in signals.items():
        vol = volatilities.get(symbol)
        if (
            isinstance(signal, numbers.Real)
            and not isinstance(signal, bool)
            and math.isfinite(signal)
            and isinstance(vol, numbers.Real)
            and not isinstance(vol, bool)
            and math.isfinite(vol)
            and vol > 0
        ):
            raw[symbol] = float(np.clip(float(signal) * target_vol / float(vol), -max_weight, max_weight))
        else:
            raw[symbol] = 0.0
    gross = sum(abs(v) for v in raw.values())
    scale = min(1.0, gross_weight / gross) if gross > 0 else 1.0
    # Round toward zero so wire precision never exceeds the declared gross budget.
    return {
        symbol: float(Decimal(str(value * scale)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
        for symbol, value in raw.items()
    }


def cap_sector_weights(
    weights: dict,
    max_sector_gross: float = 0.4,
    max_sector_net: float = 0.0,
    sectors: dict | None = None,
) -> dict:
    """Cap per-sector gross and net exposure across China futures sectors without flipping signs."""
    if max_sector_gross < 0 or max_sector_net < 0:
        raise ValueError("sector limits must be nonnegative")
    out = {symbol: float(value) for symbol, value in weights.items()}
    groups: dict[str, list[str]] = {}
    for symbol in out:
        sec = sectors[symbol] if sectors and symbol in sectors else (product_sector(symbol) or "other")
        groups.setdefault(sec, []).append(symbol)
    for members in groups.values():
        if max_sector_gross > 0:
            gross = sum(abs(out[s]) for s in members)
            if gross > max_sector_gross:
                factor = max_sector_gross / gross
                for s in members:
                    out[s] *= factor
        if max_sector_net > 0:
            net = sum(out[s] for s in members)
            if abs(net) > max_sector_net:
                # Trim only the dominant side so opposite hedges are preserved and no sign flips.
                dom = [s for s in members if (out[s] > 0 if net > 0 else out[s] < 0)]
                opp_abs = sum(abs(out[s]) for s in members if s not in dom)
                dom_abs = sum(abs(out[s]) for s in dom)
                target_dom_abs = opp_abs + max_sector_net
                factor = min(1.0, max(0.0, target_dom_abs / dom_abs)) if dom_abs > 0 else 1.0
                for s in dom:
                    out[s] *= factor
    return {
        symbol: float(Decimal(str(value)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
        for symbol, value in out.items()
    }


def smooth_weights(
    previous: dict,
    target: dict,
    alpha: float = 0.5,
    deadband: float = 0.0,
) -> dict:
    """Blend `target` with `previous` via `alpha` and suppress same-sign deltas under `deadband`."""
    if not (0.0 < alpha <= 1.0) or deadband < 0.0:
        raise ValueError("alpha must be in (0, 1] and deadband must be nonnegative")
    symbols = set(previous) | set(target)
    out = {}
    for symbol in symbols:
        prev = float(previous.get(symbol, 0.0))
        tgt = float(target.get(symbol, 0.0))
        blended = alpha * tgt + (1.0 - alpha) * prev
        # Never suppress opening from flat, closing to flat, or reversing side.
        same_side = prev != 0.0 and tgt != 0.0 and (prev > 0) == (tgt > 0)
        if deadband > 0.0 and same_side and abs(blended - prev) < deadband:
            out[symbol] = prev
        else:
            out[symbol] = blended
    return out


def drawdown_scale(
    equity_curve,
    soft_dd: float = 0.05,
    hard_dd: float = 0.15,
    min_scale: float = 0.25,
) -> float:
    """Linear risk multiplier in `[min_scale, 1.0]` from trailing peak-to-trough drawdown."""
    if not (0.0 <= soft_dd < hard_dd < 1.0) or not (0.0 <= min_scale <= 1.0):
        raise ValueError("require 0 <= soft_dd < hard_dd < 1 and 0 <= min_scale <= 1")
    arr = np.asarray(equity_curve, dtype=float)
    finite = arr[np.isfinite(arr) & (arr > 0)]
    if len(finite) == 0:
        return 1.0
    peak = float(np.max(finite))
    current = float(finite[-1])
    dd = max(0.0, 1.0 - current / peak)
    if dd <= soft_dd:
        return 1.0
    if dd >= hard_dd:
        return float(min_scale)
    frac = (dd - soft_dd) / (hard_dd - soft_dd)
    return float(1.0 - frac * (1.0 - min_scale))


def combine_weights(allocations: list, symbols) -> dict:
    """Linearly combine `[(weights_by_symbol, share), ...]` across `symbols`."""
    combined = {symbol: 0.0 for symbol in symbols}
    for weights, share in allocations:
        s = float(share)
        if s == 0.0:
            continue
        for symbol, w in weights.items():
            if symbol in combined:
                combined[symbol] += s * float(w)
    return combined


def ledoit_wolf_cov(returns) -> np.ndarray:
    """Linear shrinkage covariance estimator toward the scaled identity matrix.

    Reference:
        Ledoit, O., & Wolf, M. (2004). A well-conditioned estimator for
        large-dimensional covariance matrices. Journal of Multivariate Analysis,
        88(2), 365-411. https://doi.org/10.1016/S0047-259X(03)00096-4

    Equations:
        S = (1 / T) * Y^T Y,  m = tr(S) / N,  d^2 = ||S - m I||_F^2 / N,
        b_bar^2 = (1 / (N T^2)) * sum_t ||y_t y_t^T - S||_F^2,
        b^2 = min(b_bar^2, d^2),  delta = b^2 / d^2,
        Sigma_hat = delta * m * I + (1 - delta) * S.

    Project adaptation:
        Rows with non-finite entries are dropped before estimation; strictly
        causal when fed trailing return history through ctx.time.
    """
    arr = np.asarray(returns, dtype=float)
    if arr.ndim != 2:
        raise ValueError("returns must be a 2D array of shape (observations, assets)")
    arr = arr[np.all(np.isfinite(arr), axis=1)]
    t_obs, n_assets = arr.shape
    if n_assets < 1 or t_obs < 2:
        raise ValueError("ledoit_wolf_cov requires at least 2 finite observations and 1 asset")
    centered = arr - np.mean(arr, axis=0, keepdims=True)
    sample = (centered.T @ centered) / float(t_obs)
    if n_assets == 1:
        return sample
    mu = float(np.trace(sample)) / float(n_assets)
    target = mu * np.eye(n_assets, dtype=float)
    diff = sample - target
    d2 = float(np.sum(diff * diff)) / float(n_assets)
    if d2 <= 1e-24:
        return 0.5 * (sample + sample.T)
    row_norm_sq = np.sum(centered * centered, axis=1)
    sample_frob_sq = float(np.sum(sample * sample))
    b_bar2 = max(0.0, float(np.sum(row_norm_sq * row_norm_sq)) - float(t_obs) * sample_frob_sq) / (
        float(n_assets) * float(t_obs * t_obs)
    )
    delta = min(b_bar2, d2) / d2
    shrunk = delta * target + (1.0 - delta) * sample
    return 0.5 * (shrunk + shrunk.T)


def _ivp_cluster_var(cov: np.ndarray, indices: list[int]) -> float:
    sub = cov[np.ix_(indices, indices)]
    diag = np.maximum(np.diag(sub), 1e-18)
    inv = 1.0 / diag
    w = inv / float(np.sum(inv))
    return float(w @ sub @ w)


def hrp_weights(
    returns,
    *,
    shrinkage: bool = True,
    linkage_method: str = "single",
):
    """Hierarchical Risk Parity (HRP) long-only allocation weights summing to 1.

    Reference:
        López de Prado, M. (2016). Building Diversified Portfolios that
        Outperform Out-of-Sample. The Journal of Portfolio Management, 42(4),
        59-69. https://doi.org/10.3905/jpm.2016.42.4.059

    Equations:
        1. Correlation distance: d_{i,j} = sqrt(0.5 * (1 - rho_{i,j})).
        2. Hierarchical clustering & quasi-diagonalization via dendrogram leaves.
        3. Recursive bisection with inverse-variance cluster risk:
           alpha = 1 - V_left / (V_left + V_right).

    Project adaptation:
        Optionally estimates the covariance matrix with Ledoit-Wolf (2004)
        linear shrinkage (`shrinkage=True` by default) to stabilize small-window
        futures cross-sections; accepts either a `{name: series}` dict or a 2D
        `(observations, assets)` array.
    """
    from scipy.cluster.hierarchy import leaves_list, linkage
    from scipy.spatial.distance import squareform

    names = None
    if isinstance(returns, dict):
        names = list(returns.keys())
        if not names:
            return {}
        arr = np.column_stack([np.asarray(returns[name], dtype=float) for name in names])
    else:
        arr = np.asarray(returns, dtype=float)
        if arr.ndim != 2:
            raise ValueError("returns must be a {name: series} dict or 2D (T, N) array")
    n_assets = arr.shape[1]
    if n_assets == 0:
        return {} if names is not None else np.empty(0, dtype=float)
    if n_assets == 1:
        return {names[0]: 1.0} if names is not None else np.array([1.0], dtype=float)

    finite_rows = arr[np.all(np.isfinite(arr), axis=1)]
    if len(finite_rows) < 2:
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    cov = ledoit_wolf_cov(finite_rows) if shrinkage else np.cov(finite_rows, rowvar=False, ddof=1)
    cov = 0.5 * (np.asarray(cov, dtype=float) + np.asarray(cov, dtype=float).T)
    diag = np.diag(cov)
    if not np.all(np.isfinite(cov)) or np.any(diag <= 1e-18):
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    std = np.sqrt(diag)
    corr = np.clip(cov / np.outer(std, std), -1.0, 1.0)
    np.fill_diagonal(corr, 1.0)
    dist = np.sqrt(np.maximum(0.0, 0.5 * (1.0 - corr)))
    dist = 0.5 * (dist + dist.T)
    np.fill_diagonal(dist, 0.0)
    if n_assets == 2:
        order = [0, 1]
    else:
        condensed = squareform(dist, checks=False)
        order = [int(idx) for idx in leaves_list(linkage(condensed, method=linkage_method))]

    weights = np.ones(n_assets, dtype=float)
    clusters = [order]
    while clusters:
        next_clusters = []
        for cluster in clusters:
            if len(cluster) <= 1:
                continue
            mid = len(cluster) // 2
            left, right = cluster[:mid], cluster[mid:]
            v_left = _ivp_cluster_var(cov, left)
            v_right = _ivp_cluster_var(cov, right)
            denom = v_left + v_right
            alpha = 0.5 if denom <= 0.0 or not math.isfinite(denom) else (1.0 - v_left / denom)
            weights[left] *= alpha
            weights[right] *= (1.0 - alpha)
            if len(left) > 1:
                next_clusters.append(left)
            if len(right) > 1:
                next_clusters.append(right)
        clusters = next_clusters

    total = float(np.sum(weights))
    weights = weights / total if total > 0.0 and math.isfinite(total) else np.full(n_assets, 1.0 / n_assets)
    if names is not None:
        return {name: float(weights[i]) for i, name in enumerate(names)}
    return weights


def _project_capped_simplex(values: np.ndarray, upper: float) -> np.ndarray:
    """Euclidean projection onto `{w : 0 <= w_i <= upper, sum(w) = 1}` via bisection."""
    lo = float(np.min(values)) - upper - 1.0
    hi = float(np.max(values)) + 1.0
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if float(np.sum(np.clip(values - mid, 0.0, upper))) > 1.0:
            lo = mid
        else:
            hi = mid
    w = np.clip(values - 0.5 * (lo + hi), 0.0, upper)
    s = float(np.sum(w))
    return w / s if s > 0.0 else np.full(len(values), 1.0 / len(values))


def erc_weights(
    returns,
    *,
    shrinkage: bool = True,
    is_cov: bool = False,
    max_weight: float = 1.0,
):
    """Equal Risk Contribution (Risk Parity) long-only weights summing to 1.

    References:
        Maillard, S., Roncalli, T., & Teïletche, J. (2010). The Properties of
        Equally Weighted Risk Contribution Portfolios. The Journal of Portfolio
        Management, 36(4), 60-70. https://doi.org/10.3905/jpm.2010.36.4.060
        Spinu, F. (2013). An Algorithm for Computing Risk Parity Weights.
        SSRN 2297383. https://ssrn.com/abstract=2297383

    Equations:
        Unconstrained convex log-barrier formulation (Spinu 2013) in u = ln(y):
            min_u  0.5 * y^T Sigma y - (1 / N) * sum(u_i),   y = exp(u),
            grad = y * (Sigma y) - (1 / N),   w* = y* / sum(y*).
        At optimality, w_i (Sigma w)_i / (w^T Sigma w) = 1 / N for all i.

    Project adaptation:
        Estimates covariance with Ledoit-Wolf (2004) shrinkage by default
        (`shrinkage=True`) or accepts a precomputed covariance matrix
        (`is_cov=True`); when `max_weight < 1.0` binds, refines on the capped
        simplex by minimizing squared risk-contribution deviations via SLSQP.
    """
    from scipy.optimize import minimize

    if (
        not isinstance(max_weight, numbers.Real)
        or isinstance(max_weight, bool)
        or not math.isfinite(max_weight)
        or float(max_weight) <= 0.0
    ):
        raise ValueError("max_weight must be a positive finite number")
    cap = float(max_weight)
    names = None
    if is_cov:
        cov = np.asarray(returns, dtype=float)
        if cov.ndim != 2 or cov.shape[0] != cov.shape[1]:
            raise ValueError("covariance matrix must be a square 2D array")
        n_assets = cov.shape[0]
        if n_assets == 0:
            return np.empty(0, dtype=float)
        if cap * n_assets < 1.0 - 1e-12:
            raise ValueError("max_weight * number of assets must be at least 1.0")
        if n_assets == 1:
            return np.array([1.0], dtype=float)
        cov = 0.5 * (cov + cov.T)
    else:
        if isinstance(returns, dict):
            names = list(returns.keys())
            if not names:
                return {}
            arr = np.column_stack([np.asarray(returns[name], dtype=float) for name in names])
        else:
            arr = np.asarray(returns, dtype=float)
            if arr.ndim != 2:
                raise ValueError("returns must be a {name: series} dict or 2D (T, N) array")
        n_assets = arr.shape[1]
        if n_assets == 0:
            return {} if names is not None else np.empty(0, dtype=float)
        if cap * n_assets < 1.0 - 1e-12:
            raise ValueError("max_weight * number of assets must be at least 1.0")
        if n_assets == 1:
            return {names[0]: 1.0} if names is not None else np.array([1.0], dtype=float)
        finite_rows = arr[np.all(np.isfinite(arr), axis=1)]
        if len(finite_rows) < 2:
            eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
            return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq
        cov = ledoit_wolf_cov(finite_rows) if shrinkage else np.cov(finite_rows, rowvar=False, ddof=1)
        cov = 0.5 * (np.asarray(cov, dtype=float) + np.asarray(cov, dtype=float).T)

    diag = np.diag(cov)
    if not np.all(np.isfinite(cov)) or np.any(diag <= 1e-18):
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    eig_min = float(np.min(np.linalg.eigvalsh(cov)))
    if eig_min <= 1e-12:
        cov = cov + (1e-12 - eig_min) * np.eye(n_assets, dtype=float)
    scale = float(np.trace(cov)) / float(n_assets)
    cov_s = cov / scale

    inv_vol = 1.0 / np.sqrt(np.diag(cov_s))
    y0 = inv_vol / math.sqrt(float(inv_vol @ cov_s @ inv_vol))
    u0 = np.log(np.maximum(y0, 1e-12))
    target_rc = 1.0 / float(n_assets)

    def spinu_obj(u: np.ndarray) -> tuple[float, np.ndarray]:
        uc = np.clip(u, -30.0, 30.0)
        y = np.exp(uc)
        sy = cov_s @ y
        val = 0.5 * float(y @ sy) - target_rc * float(np.sum(uc))
        grad = y * sy - target_rc
        return val, grad

    res = minimize(spinu_obj, u0, method="L-BFGS-B", jac=True, options={"ftol": 1e-15, "gtol": 1e-12})
    y_opt = np.exp(np.clip(res.x, -30.0, 30.0))
    weights = y_opt / float(np.sum(y_opt))

    if cap < 1.0 - 1e-12 and float(np.max(weights)) > cap + 1e-9:
        w0 = _project_capped_simplex(weights, cap)

        def rc_obj(w: np.ndarray) -> tuple[float, np.ndarray]:
            sw = cov_s @ w
            port_var = float(w @ sw)
            if port_var <= 1e-24:
                return 0.0, np.zeros_like(w)
            rc = (w * sw) / port_var
            diff = rc - target_rc
            val = float(np.sum(diff * diff))
            # d(rc_i)/d(w_j) = (delta_ij (Sigma w)_i + w_i Sigma_ij) / V - 2 rc_i (Sigma w)_j / V.
            grad = (2.0 / port_var) * (diff * sw + cov_s @ (diff * w) - 2.0 * float(diff @ rc) * sw)
            return val, grad

        capped = minimize(
            rc_obj,
            w0,
            method="SLSQP",
            jac=True,
            bounds=[(1e-9, cap)] * n_assets,
            constraints=[{"type": "eq", "fun": lambda w: float(np.sum(w)) - 1.0, "jac": lambda w: np.ones(n_assets)}],
            options={"ftol": 1e-14, "maxiter": 300},
        )
        if capped.success and np.all(np.isfinite(capped.x)):
            weights = _project_capped_simplex(capped.x, cap)
        else:
            weights = w0

    if names is not None:
        return {name: float(weights[i]) for i, name in enumerate(names)}
    return weights


def mvo_weights(
    expected_returns,
    cov,
    *,
    previous_weights=None,
    risk_aversion: float = 1.0,
    turnover_cost: float = 0.0,
    gross_limit: float = 1.0,
    net_limit: float | None = None,
    max_weight: float = 0.25,
    long_only: bool = False,
    sectors: dict | None = None,
    max_sector_gross: float = 0.0,
):
    """Turnover-aware constrained Mean-Variance portfolio optimizer (convex QP).

    References:
        Markowitz, H. (1952). Portfolio Selection. The Journal of Finance,
        7(1), 77-91. https://doi.org/10.2307/2975974
        Boyd, S., Busseti, E., Diamond, S., Kahn, R. N., Koh, K., Nystrup, P.,
        & Speth, J. (2017). Multi-Period Trading via Convex Optimization.
        Foundations and Trends in Optimization, 3(1), 1-76.
        https://doi.org/10.1561/2400000023

    Equations:
        min_w  0.5 * lambda * w^T Sigma w - mu^T w + c * ||w - w_prev||_1
        s.t.   |w_i| <= max_weight (w_i >= 0 if long_only),
               sum |w_i| <= gross_limit,  |sum w_i| <= net_limit,
               sum_{i in sector} |w_i| <= max_sector_gross.

    Project adaptation:
        Uses smooth nonnegative split variables w = w+ - w- (and trade splits
        w - w_prev = u+ - u- when turnover_cost > 0) solved by SLSQP with exact
        analytical gradients and Jacobians; outputs are quantized toward zero
        to WEIGHT_STEP so wire precision never exceeds exposure limits.
    """
    from scipy.optimize import minimize

    for label, value, allow_zero in (
        ("risk_aversion", risk_aversion, False),
        ("turnover_cost", turnover_cost, True),
        ("gross_limit", gross_limit, False),
        ("max_weight", max_weight, False),
        ("max_sector_gross", max_sector_gross, True),
    ):
        if (
            not isinstance(value, numbers.Real)
            or isinstance(value, bool)
            or not math.isfinite(value)
            or (float(value) < 0.0 if allow_zero else float(value) <= 0.0)
        ):
            raise ValueError(f"{label} must be a {'nonnegative' if allow_zero else 'positive'} finite number")
    if net_limit is not None and (
        not isinstance(net_limit, numbers.Real)
        or isinstance(net_limit, bool)
        or not math.isfinite(net_limit)
        or float(net_limit) < 0.0
    ):
        raise ValueError("net_limit must be None or a nonnegative finite number")

    names = None
    if isinstance(expected_returns, dict):
        names = list(expected_returns.keys())
        if not names:
            return {}
        mu_raw = np.array([
            float(expected_returns[s])
            if isinstance(expected_returns[s], numbers.Real) and not isinstance(expected_returns[s], bool)
            else math.nan
            for s in names
        ], dtype=float)
    else:
        mu_raw = np.asarray(expected_returns, dtype=float)
        if mu_raw.ndim != 1:
            raise ValueError("expected_returns must be a {symbol: return} dict or 1D array")
    n = len(mu_raw)
    if n == 0:
        return {} if names is not None else np.empty(0, dtype=float)

    cov_arr = np.asarray(cov, dtype=float)
    if cov_arr.shape != (n, n) or not np.all(np.isfinite(cov_arr)):
        raise ValueError(f"cov must be a finite ({n}, {n}) matrix")
    cov_arr = 0.5 * (cov_arr + cov_arr.T)
    eig_min = float(np.min(np.linalg.eigvalsh(cov_arr)))
    if eig_min < 0.0:
        cov_arr = cov_arr + (-eig_min + 1e-12) * np.eye(n, dtype=float)

    if previous_weights is None:
        w_prev = np.zeros(n, dtype=float)
    elif isinstance(previous_weights, dict):
        if names is None:
            raise ValueError("previous_weights dict requires expected_returns dict")
        w_prev = np.array([float(previous_weights.get(s, 0.0)) for s in names], dtype=float)
    else:
        w_prev = np.asarray(previous_weights, dtype=float)
        if w_prev.shape != (n,):
            raise ValueError(f"previous_weights must have length {n}")
    if not np.all(np.isfinite(w_prev)):
        raise ValueError("previous_weights must be finite")

    valid_mu = np.isfinite(mu_raw)
    mu = np.where(valid_mu, mu_raw, 0.0)
    lam = float(risk_aversion)
    tc = float(turnover_cost)
    g_lim = float(gross_limit)
    w_cap = float(max_weight)
    sec_cap = float(max_sector_gross)
    eps_tie = 1e-10

    sector_groups: list[list[int]] = []
    if sec_cap > 0.0:
        groups: dict[str, list[int]] = {}
        for i in range(n):
            if names is not None:
                sym = names[i]
                sec = sectors[sym] if sectors and sym in sectors else (product_sector(sym) or "other")
            else:
                sec = str(sectors[i]) if sectors and i in sectors else "other"
            groups.setdefault(sec, []).append(i)
        sector_groups = list(groups.values())

    n_vars = 4 * n if tc > 0.0 else 2 * n
    bounds = []
    for i in range(n):
        hi = w_cap if valid_mu[i] else 0.0
        bounds.append((0.0, hi))
    for i in range(n):
        hi = 0.0 if (long_only or not valid_mu[i]) else w_cap
        bounds.append((0.0, hi))
    if tc > 0.0:
        bounds.extend([(0.0, None)] * (2 * n))

    def objective(z: np.ndarray) -> tuple[float, np.ndarray]:
        wp = z[:n]
        wm = z[n:2 * n]
        w = wp - wm
        cw = cov_arr @ w
        val = 0.5 * lam * float(w @ cw) - float(mu @ w) + eps_tie * float(np.sum(wp) + np.sum(wm))
        gw = lam * cw - mu
        if tc > 0.0:
            up = z[2 * n:3 * n]
            um = z[3 * n:4 * n]
            val += tc * float(np.sum(up) + np.sum(um))
            grad = np.concatenate([gw + eps_tie, -gw + eps_tie, np.full(2 * n, tc)])
        else:
            grad = np.concatenate([gw + eps_tie, -gw + eps_tie])
        return val, grad

    constraints = []
    # Gross exposure: sum(w+ + w-) <= gross_limit
    jac_gross = np.zeros(n_vars, dtype=float)
    jac_gross[:2 * n] = -1.0
    constraints.append({
        "type": "ineq",
        "fun": lambda z: g_lim - float(np.sum(z[:2 * n])),
        "jac": lambda _z, j=jac_gross: j,
    })

    if net_limit is not None:
        n_lim = float(net_limit)
        jac_net_hi = np.zeros(n_vars, dtype=float)
        jac_net_hi[:n] = -1.0
        jac_net_hi[n:2 * n] = 1.0
        jac_net_lo = -jac_net_hi
        constraints.append({
            "type": "ineq",
            "fun": lambda z, lim=n_lim: lim - float(np.sum(z[:n]) - np.sum(z[n:2 * n])),
            "jac": lambda _z, j=jac_net_hi: j,
        })
        constraints.append({
            "type": "ineq",
            "fun": lambda z, lim=n_lim: lim + float(np.sum(z[:n]) - np.sum(z[n:2 * n])),
            "jac": lambda _z, j=jac_net_lo: j,
        })

    for idxs in sector_groups:
        idx_arr = np.array(idxs, dtype=int)
        jac_sec = np.zeros(n_vars, dtype=float)
        jac_sec[idx_arr] = -1.0
        jac_sec[n + idx_arr] = -1.0
        constraints.append({
            "type": "ineq",
            "fun": lambda z, ia=idx_arr: sec_cap - float(np.sum(z[ia]) + np.sum(z[n + ia])),
            "jac": lambda _z, j=jac_sec: j,
        })

    if tc > 0.0:
        eye_n = np.eye(n, dtype=float)
        jac_trade = np.hstack([eye_n, -eye_n, -eye_n, eye_n])
        constraints.append({
            "type": "eq",
            "fun": lambda z: (z[:n] - z[n:2 * n]) - (z[2 * n:3 * n] - z[3 * n:4 * n]) - w_prev,
            "jac": lambda _z, j=jac_trade: j,
        })

    z0 = np.zeros(n_vars, dtype=float)
    if tc > 0.0:
        # w = 0 is always feasible: u_minus = max(0, w_prev), u_plus = max(0, -w_prev).
        z0[2 * n:3 * n] = np.maximum(0.0, -w_prev)
        z0[3 * n:4 * n] = np.maximum(0.0, w_prev)

    sol = minimize(
        objective,
        z0,
        method="SLSQP",
        jac=True,
        bounds=bounds,
        constraints=constraints,
        options={"ftol": 1e-12, "maxiter": 400},
    )
    z_opt = sol.x if np.all(np.isfinite(sol.x)) else z0
    w_opt = z_opt[:n] - z_opt[n:2 * n]
    w_opt = np.where(np.abs(w_opt) < 1e-10, 0.0, w_opt)
    w_quant = np.array(
        [float(Decimal(str(val)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN)) for val in w_opt],
        dtype=float,
    )
    if names is not None:
        return {name: float(w_quant[i]) for i, name in enumerate(names)}
    return w_quant


class _SubView:
    def __init__(self, parent: Context):
        self.parent = parent
        self.mode = None
        self.wanted = {}

    @property
    def symbols(self):
        return self.parent.symbols

    @property
    def time(self):
        return self.parent.time

    @property
    def contracts(self):
        return self.parent.contracts

    def history(self, symbol: str, field: str, count: int) -> np.ndarray:
        return self.parent.history(symbol, field, count)

    def factor(self, function, symbol: str, count: int, **settings) -> float:
        return self.parent.factor(function, symbol, count, **settings)

    def weights(self, weights: dict):
        if self.mode not in (None, "weights"):
            raise RuntimeError("CombinedStrategy sub-strategies must output weights")
        self.mode = "weights"
        out = {}
        for symbol, weight in weights.items():
            if symbol not in self.parent.columns:
                raise KeyError(f"{symbol} is not traded by this run")
            if not isinstance(weight, numbers.Real) or isinstance(weight, bool) \
                    or not math.isfinite(weight):
                raise TypeError(f"weight of {symbol} is not a finite number: {weight!r}")
            out[symbol] = float(weight)
        self.wanted = {symbol: out.get(symbol, 0.0) for symbol in self.parent.symbols}

    def target(self, symbol: str, lots: int):
        raise RuntimeError("CombinedStrategy sub-strategies must output weights, not lots")


class CombinedStrategy(Strategy):
    """Standard single-instance combiner merging target weights of sub-strategies.

    Subclasses may set `allocation` to `"fixed"`, `"vol_balance"` (or
    `vol_balance=True`), `"hrp"` (causal Hierarchical Risk Parity with
    Ledoit-Wolf shrinkage across component proxy returns), or `"erc"` (causal
    Equal Risk Contribution / Risk Parity with Ledoit-Wolf shrinkage),
    `max_sector_gross` / `max_sector_net` for sector exposure caps, and
    `smooth_alpha` / `deadband` for portfolio-level turnover control.
    """

    allocation: str = "fixed"
    vol_balance: bool = False
    vol_window: int = 20
    max_sector_gross: float = 0.0
    max_sector_net: float = 0.0
    smooth_alpha: float = 1.0
    deadband: float = 0.0

    def components(self) -> list:
        raise NotImplementedError

    def __init__(self, **values):
        super().__init__(**values)
        if self.allocation not in ("fixed", "vol_balance", "hrp", "erc"):
            raise ValueError("CombinedStrategy allocation must be 'fixed', 'vol_balance', 'hrp', or 'erc'")
        if self.allocation in ("hrp", "erc"):
            self._alloc_mode = self.allocation
        elif self.vol_balance or self.allocation == "vol_balance":
            self._alloc_mode = "vol_balance"
        else:
            self._alloc_mode = "fixed"
        items = list(self.components())
        if not items:
            raise ValueError("CombinedStrategy requires at least one component strategy")
        self._subs = []
        for sub, share in items:
            if not isinstance(sub, Strategy):
                raise TypeError("each component must be a Strategy instance")
            if not isinstance(share, numbers.Real) or isinstance(share, bool) \
                    or not math.isfinite(share) or share < 0:
                raise ValueError(f"component share must be a finite nonnegative number: {share!r}")
            self._subs.append((sub, float(share)))
        self._standing = [{} for _ in self._subs]
        self._was_ready = [False for _ in self._subs]
        self._all_ready = False
        self._comp_returns: list[list[float]] = [[] for _ in self._subs]
        self._prev_combined: dict[str, float] = {}
        self.last_components: list[dict] = []
        if type(self).history_limit is None:
            sub_limits = [getattr(sub, "history_limit", None) for sub, _ in self._subs]
            if all(limit is not None for limit in sub_limits):
                self.history_limit = max(2, max(sub_limits))

    def ready(self, ctx: Context) -> bool:
        return self._all_ready

    def _effective_shares(self) -> list[float]:
        base = [share for _, share in self._subs]
        if self._alloc_mode == "fixed":
            return base
        window = max(2, int(self.vol_window))
        total_share = sum(base)
        if total_share <= 0:
            return base
        active = [idx for idx, share in enumerate(base) if share > 0]
        if self._alloc_mode in ("hrp", "erc"):
            if len(active) < 2:
                return base
            cov_window = max(5, window)
            if any(len(self._comp_returns[idx]) < cov_window for idx in active):
                return base
            matrix = np.column_stack([
                np.asarray(self._comp_returns[idx][-cov_window:], dtype=float)
                for idx in active
            ])
            raw_alloc = (
                hrp_weights(matrix, shrinkage=True)
                if self._alloc_mode == "hrp"
                else erc_weights(matrix, shrinkage=True)
            )
            tilted = [base[idx] * float(raw_alloc[pos]) for pos, idx in enumerate(active)]
            denom = sum(tilted)
            if denom <= 0 or not math.isfinite(denom):
                return base
            out = [0.0 for _ in base]
            for pos, idx in enumerate(active):
                out[idx] = total_share * tilted[pos] / denom
            return out
        inv_vols = []
        for idx, share in enumerate(base):
            if share <= 0:
                inv_vols.append(0.0)
                continue
            hist = self._comp_returns[idx]
            if len(hist) < window:
                return base
            vol = float(np.std(hist[-window:], ddof=1))
            if not math.isfinite(vol) or vol <= 1e-12:
                return base
            inv_vols.append(share / vol)
        denom = sum(inv_vols)
        if denom <= 0:
            return base
        return [total_share * w / denom for w in inv_vols]

    def on_bars(self, ctx: Context, bars: dict):
        # Update strictly causal component return proxies from roll-adjusted closes before stepping.
        if self._alloc_mode != "fixed":
            bar_returns = {}
            for sym in bars:
                closes = ctx.history(sym, "close", 2)
                if len(closes) == 2 and closes[0] > 0 and math.isfinite(closes[0]) and math.isfinite(closes[1]):
                    bar_returns[sym] = float(closes[1] / closes[0] - 1.0)
            if bar_returns:
                max_keep = max(256, 4 * max(2, int(self.vol_window)))
                for idx in range(len(self._subs)):
                    if self._was_ready[idx] and self._standing[idx]:
                        ret = sum(
                            w * bar_returns[s]
                            for s, w in self._standing[idx].items()
                            if s in bar_returns
                        )
                        hist = self._comp_returns[idx]
                        hist.append(ret)
                        if len(hist) > max_keep:
                            del hist[:-max_keep]

        changed = False
        ready_flags = []
        for idx, (sub, _) in enumerate(self._subs):
            view = _SubView(ctx)
            sub.on_bars(view, bars)
            is_ready = sub.ready(view)
            if type(is_ready) is not bool:
                raise TypeError("strategy readiness must be a bool")
            ready_flags.append(is_ready)
            if not is_ready:
                if self._standing[idx] or self._was_ready[idx]:
                    changed = True
                self._standing[idx] = {}
                self._was_ready[idx] = False
                continue
            if not self._was_ready[idx]:
                self._was_ready[idx] = True
                changed = True
            if view.wanted:
                self._standing[idx] = view.wanted
                changed = True
        self._all_ready = all(ready_flags)
        shares = self._effective_shares()
        self.last_components = [
            {
                "name": type(sub).__name__,
                "share": shares[idx],
                "ready": bool(self._was_ready[idx]),
                "weights": dict(self._standing[idx]),
            }
            for idx, (sub, _) in enumerate(self._subs)
        ]
        if self._all_ready and (changed or self._alloc_mode != "fixed"):
            combined = combine_weights(
                [(self._standing[i], shares[i]) for i in range(len(self._subs))],
                ctx.symbols,
            )
            if self.max_sector_gross > 0 or self.max_sector_net > 0:
                combined = cap_sector_weights(
                    combined,
                    max_sector_gross=self.max_sector_gross,
                    max_sector_net=self.max_sector_net,
                )
            if self.smooth_alpha < 1.0 or self.deadband > 0.0:
                if self._prev_combined:
                    combined = smooth_weights(
                        self._prev_combined,
                        combined,
                        alpha=self.smooth_alpha,
                        deadband=self.deadband,
                    )
                self._prev_combined = dict(combined)
            ctx.weights(combined)
