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
from decimal import Decimal, ROUND_HALF_EVEN
import inspect
import math
import numbers

import numpy as np

FIELDS = ("open", "high", "low", "close", "volume", "open_interest", "roll_yield")
PRICES = ("open", "high", "low", "close")
KINDS = {bool: "bool", int: "int", float: "float", str: "str"}
WEIGHT_STEP = Decimal("0.00000001")


def quantize_weight(value: float) -> Decimal:
    # Convert via str() with explicit ROUND_HALF_EVEN so backtest, rolling and live agree.
    return Decimal(str(value)).quantize(WEIGHT_STEP, rounding=ROUND_HALF_EVEN)


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


class Strategy:
    """The base of every strategy. `params` names each parameter with its
    default; a run may set any of them to a value of the same kind."""
    params: dict = {}

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        for name, value in cls.params.items():
            if not isinstance(name, str) or not name.isidentifier():
                raise TypeError(f"{cls.__qualname__}: parameter name {name!r} is not an identifier")
            if type(value) not in KINDS:
                raise TypeError(f"{cls.__qualname__}: parameter {name} defaults to a "
                                f"{type(value).__name__}; use bool, int, float or str")
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
    """A growing array whose filled part is read without copying."""

    def __init__(self):
        self.values = np.empty(256)
        self.size = 0

    def append(self, value: float):
        if self.size == len(self.values):
            self.values = np.concatenate([self.values, np.empty(len(self.values))])
        self.values[self.size] = value
        self.size += 1

    def scale(self, ratio: float):
        # A new array: views handed out before keep what they showed.
        scaled = np.empty_like(self.values)
        scaled[:self.size] = self.values[:self.size] * ratio
        self.values = scaled

    def last(self, count: int) -> np.ndarray:
        view = self.values[max(0, self.size - count):self.size]
        view.flags.writeable = False
        return view


class Context:
    """A run's view for its strategy: the bars so far and what it wants to hold.
    It offers no way to send an order."""

    def __init__(self, symbols, rolls=None):
        """`rolls`: for each month a dominant series rolls into, new over old
        settlement on the day before the roll."""
        self.symbols = tuple(symbols)
        if not self.symbols or len(set(self.symbols)) != len(self.symbols):
            raise ValueError("a run trades one or more distinct symbols")
        self.time = None
        self.columns = {symbol: {field: Column() for field in FIELDS} for symbol in self.symbols}
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
        if field not in FIELDS:
            raise KeyError(f"no bar field {field}")
        if not isinstance(count, numbers.Integral) or isinstance(count, bool) or count < 1:
            raise ValueError("count is a positive whole number")
        return self.columns[symbol][field].last(int(count))

    def factor(self, function, symbol: str, count: int, **settings) -> float:
        """A factor's value at the bar being decided on, computed over the
        symbol's last `count` bars: the function's parameters named after bar
        fields get that history, `settings` the rest."""
        inputs = {name: self.history(symbol, name, count)
                  for name in inspect.signature(function).parameters if name in FIELDS}
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
    """Standard single-instance combiner merging target weights of sub-strategies."""

    def components(self) -> list:
        raise NotImplementedError

    def __init__(self, **values):
        super().__init__(**values)
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

    def ready(self, ctx: Context) -> bool:
        return self._all_ready

    def on_bars(self, ctx: Context, bars: dict):
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
        if self._all_ready and changed:
            ctx.weights(combine_weights(
                [(self._standing[i], share) for i, (_, share) in enumerate(self._subs)],
                ctx.symbols))
