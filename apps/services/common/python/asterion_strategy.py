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


def ts_ewma_vol(close, span: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal EWMA volatility of log returns (`alpha = 2 / (span + 1)`)."""
    if type(span) is not int or span < 2:
        raise ValueError("span must be an integer of at least 2")
    if not isinstance(annualize, numbers.Real) or isinstance(annualize, bool) \
            or not math.isfinite(annualize) or annualize < 0:
        raise ValueError("annualize must be a nonnegative finite number")
    arr = np.asarray(close, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < 2:
        return out
    alpha = 2.0 / (span + 1.0)
    scale = math.sqrt(float(annualize)) if annualize > 0 else 1.0
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
    `vol_balance=True`), or `"hrp"` (causal Hierarchical Risk Parity with
    Ledoit-Wolf shrinkage across component proxy returns), `max_sector_gross` /
    `max_sector_net` for sector exposure caps, and `smooth_alpha` / `deadband`
    for portfolio-level turnover control.
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
        if self.allocation not in ("fixed", "vol_balance", "hrp"):
            raise ValueError("CombinedStrategy allocation must be 'fixed', 'vol_balance', or 'hrp'")
        self._alloc_mode = "hrp" if self.allocation == "hrp" else (
            "vol_balance" if (self.vol_balance or self.allocation == "vol_balance") else "fixed"
        )
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
        if self._alloc_mode == "hrp":
            if len(active) < 2:
                return base
            hrp_window = max(5, window)
            if any(len(self._comp_returns[idx]) < hrp_window for idx in active):
                return base
            matrix = np.column_stack([
                np.asarray(self._comp_returns[idx][-hrp_window:], dtype=float)
                for idx in active
            ])
            raw_hrp = hrp_weights(matrix, shrinkage=True)
            tilted = [base[idx] * float(raw_hrp[pos]) for pos, idx in enumerate(active)]
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
