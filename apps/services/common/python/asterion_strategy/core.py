"""Core strategy runtime contract: bars, bounded history, context and causality checks."""
from dataclasses import dataclass
import datetime
from decimal import Decimal, ROUND_HALF_EVEN
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
SIDES = ("both", "long", "short")
KINDS = {bool: "bool", int: "int", float: "float", str: "str"}
WEIGHT_STEP = Decimal("0.00000001")


def price_units(values) -> list[int]:
    """Convert prices to exact eight-place fixed-point integer units for rounding-free comparisons."""
    return [int(value) for value in np.rint(np.asarray(values, dtype=float) * 1e8)]


def side_lots(side: int, lots: int, sides: str = "both") -> int:
    """Map a direction sign (`-1`, `0`, `+1`) to target integer lots permitted by `sides`."""
    if lots <= 0:
        raise ValueError("lots must be positive")
    if sides not in SIDES:
        raise ValueError(f"sides is one of {', '.join(SIDES)}")
    if side > 0 and sides != "short":
        return lots
    if side < 0 and sides != "long":
        return -lots
    return 0


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
        result = np.asarray(
            function(**{name: values[begin:end] for name, values in arrays.items()}, **settings),
            dtype=float,
        )
        if result.shape != (end - begin,):
            raise ValueError(f"factor must return one value per bar, got {result.shape}")
        return result

    full = compute(0, size)
    for end in range(1, size + 1):
        prefix = compute(0, end)
        if not np.allclose(prefix, full[:end], rtol=1e-10, atol=1e-12, equal_nan=True):
            raise ValueError(f"factor changes historical values when truncated to {end} bars")
        if end > count and not np.allclose(
            compute(end - count, end)[-1], prefix[-1], rtol=1e-10, atol=1e-12, equal_nan=True
        ):
            raise ValueError(
                f"factor history window {count} differs from the full prefix at bar {end}"
            )


def scale_invariant(function):
    """Mark a price factor whose output is invariant to positive multiplicative price rescaling."""
    function.scale_invariant = True
    return function


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
    cuts = sorted(
        {
            c
            for c in (
                1,
                2,
                max(1, span - 1),
                span,
                span + 1,
                size // 2,
                size - 3,
                size - 2,
                size - 1,
            )
            if 1 <= c < size
        }
    )
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
    if getattr(function, "scale_invariant", False) and any(field in PRICES for field in inputs):
        scaled_arrays = {}
        for field, arr in arrays.items():
            scaled = arr * 2.37 if field in PRICES else arr
            if scaled is not arr:
                scaled.flags.writeable = False
            scaled_arrays[field] = scaled
        scaled_out = np.asarray(function(**scaled_arrays, **settings), dtype=float)
        if scaled_out.shape != (size,) or not np.allclose(
            scaled_out, full, rtol=1e-6, atol=1e-8, equal_nan=True
        ):
            raise ValueError(f"{label} is marked scale_invariant but changes under price rescaling")


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
                raise TypeError(
                    f"{cls.__qualname__}: parameter {name} defaults to a "
                    f"{type(value).__name__}; use bool, int, float or str"
                )
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
            arr[: self.size] = math.nan
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
                    grown[: self.size] = self.values
                    self.values = grown
                else:
                    # A new array: views handed out before keep what they showed.
                    compacted = np.empty(len(self.values))
                    keep = self.max_len
                    compacted[:keep] = self.values[self.size - keep : self.size]
                    self.values = compacted
                    self.size = keep
            else:
                grown = np.empty(len(self.values) * 2)
                grown[: self.size] = self.values
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
            scaled[:keep] = self.values[self.size - keep : self.size] * ratio
            self.size = keep
        else:
            scaled[: self.size] = self.values[: self.size] * ratio
        self.values = scaled

    def last(self, count: int | None = None) -> np.ndarray:
        if count is None:
            count = self.max_len if self.max_len is not None else self.size
        if self.max_len is not None and count > self.max_len:
            raise ValueError(f"requested {count} bars exceeds history_limit {self.max_len}")
        if self.values is None:
            view = np.full(min(count, self.size), math.nan)
            view.flags.writeable = False
            return view
        view = self.values[max(0, self.size - count) : self.size]
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
            symbol: {field: Column(max_history) for field in FIELDS} for symbol in self.symbols
        }
        self.rolls = dict(rolls or {})
        self.contracts = {}
        self.mode = None  # "target" or "weights", whichever the strategy used first
        self.wanted = {}
        self.ready = False

    def history(self, symbol: str, field: str, count: int | None = None) -> np.ndarray:
        """The last `count` values of a field, through the bar being decided on;
        fewer while there are not that many. None uses all retained history. Read-only."""
        if symbol not in self.columns:
            raise KeyError(f"{symbol} is not traded by this run")
        if field not in FIELDS_SET:
            raise KeyError(f"no bar field {field}")
        if count is not None:
            if not isinstance(count, numbers.Integral) or isinstance(count, bool) or count < 1:
                raise ValueError("count is a positive whole number")
            count = int(count)
        return self.columns[symbol][field].last(count)

    def factor(self, function, symbol: str, count: int | None = None, **settings) -> float:
        """A factor's value at the bar being decided on, computed over the
        symbol's last `count` bars: the function's parameters named after bar
        fields get that history, `settings` the rest. None uses all retained history."""
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
            if (
                not isinstance(weight, numbers.Real)
                or isinstance(weight, bool)
                or not math.isfinite(weight)
            ):
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
