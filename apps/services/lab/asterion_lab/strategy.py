"""What a strategy is written against.

A strategy is a class in the user's own strategy repository. It reads completed
bars and says what it wants to hold from the next bar on; it never sends
orders. The engine turns what it wants into whole lots and orders, applies the
risk rules and keeps every amount in fixed point.

    class MaCross(Strategy):
        \"\"\"Long above the slow average, short below it.\"\"\"
        params = {"fast": 5, "slow": 20, "lots": 1}

        def on_bar(self, ctx, bar):
            closes = ctx.history(bar.symbol, "close", self.params["slow"])
            if len(closes) < self.params["slow"]:
                return
            up = closes[-self.params["fast"]:].mean() > closes.mean()
            ctx.target(bar.symbol, self.params["lots"] if up else -self.params["lots"])

A rule over several symbols overrides on_bars instead, which sees every bar
of one time together, and may hold weights rather than lots.
"""
from dataclasses import dataclass
import datetime
import math
import numbers

import numpy as np

FIELDS = ("open", "high", "low", "close", "volume", "open_interest")
KINDS = {bool: "bool", int: "int", float: "float", str: "str"}


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

    def last(self, count: int) -> np.ndarray:
        view = self.values[max(0, self.size - count):self.size]
        view.flags.writeable = False
        return view


class Context:
    """A run's view for its strategy: the bars so far and what it wants to hold.
    It offers no way to send an order."""

    def __init__(self, symbols):
        self.symbols = tuple(symbols)
        if not self.symbols or len(set(self.symbols)) != len(self.symbols):
            raise ValueError("a run trades one or more distinct symbols")
        self.time = None
        self.columns = {symbol: {field: Column() for field in FIELDS} for symbol in self.symbols}
        self.mode = None  # "target" or "weights", whichever the strategy used first
        self.wanted = {}

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


def step(strategy: Strategy, context: Context, bars: list) -> dict:
    """Give the strategy one time's completed bars. Returns what it asked for:
    lots or weights by symbol, empty when it asked for nothing new."""
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
        for field in FIELDS:
            context.columns[symbol][field].append(getattr(bar, field))
    context.wanted = {}
    strategy.on_bars(context, by_symbol)
    return context.wanted
