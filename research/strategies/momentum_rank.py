"""A cross-sectional momentum ranking over several contracts or series."""
from collections import deque
from decimal import Decimal, ROUND_DOWN

from asterion_strategy import SIDES, WEIGHT_STEP, Strategy

from factors.price import momentum, volatility


# Jegadeesh & Titman (1993), "Returns to Buying Winners and Selling Losers", JF 48(1), 65-91: https://doi.org/10.1111/j.1540-6261.1993.tb04702.x
# Miffre & Rallis (2007), "Momentum strategies in commodity futures markets", JBF 31(6), 1863-1886: https://doi.org/10.1016/j.jbankfin.2006.10.020
# Adaptation: synchronized shared-bar ranking across futures symbols with optional reversal sign and inverse-volatility sizing.
class MomentumRank(Strategy):
    """On every `rebalance`-th time all symbols have a bar, rank them by the
    return over `lookback` such bars: the `count` highest are held long and the
    `count` lowest short, or the other way round when `reversed`. Each held
    symbol is `weight` of equity; with a `volatility` window the held symbols
    share that by the inverse of the standard deviation of their returns."""
    params = {"lookback": 20, "rebalance": 5, "count": 1, "weight": 0.1, "volatility": 0,
              "reversed": False, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        self.need = max(p["lookback"] + 1, p["volatility"] + 1 if p["volatility"] else 0)
        self.indices = None
        self.shared = 0

    def ready(self, ctx):
        return self.shared >= self.need

    def on_bars(self, ctx, bars):
        p = self.params
        if p["sides"] not in SIDES or p["lookback"] < 1 or p["rebalance"] < 1 \
                or p["count"] < 1 or len(ctx.symbols) < 2 * p["count"] \
                or p["weight"] <= 0 or p["volatility"] < 0 or p["volatility"] == 1:
            raise ValueError("invalid ranking parameters")
        if self.indices is None:
            self.indices = {symbol: deque(maxlen=self.need) for symbol in ctx.symbols}
        # Only the times every symbol has a bar count.
        if len(bars) != len(ctx.symbols):
            return
        for symbol, bar in bars.items():
            if bar.close <= 0:
                raise ValueError("a ranking needs positive closes")
            self.indices[symbol].append(len(ctx.history(symbol, "close", 1 << 30)) - 1)
        self.shared += 1
        if self.shared < self.need or (self.shared - self.need) % p["rebalance"]:
            return
        closes = {symbol: ctx.history(symbol, "close", 1 << 30)[list(indices)]
                  for symbol, indices in self.indices.items()}
        # The factors the factor view evaluates, at this bar.
        score = {symbol: momentum(values, p["lookback"])[-1] for symbol, values in closes.items()}
        # Equal scores keep the order the symbols were given in.
        order = sorted(ctx.symbols, key=lambda symbol: score[symbol], reverse=not p["reversed"])
        wanted = dict.fromkeys(ctx.symbols, 0.0)
        for i in range(p["count"]):
            if p["sides"] != "short":
                wanted[order[i]] = 1.0
            if p["sides"] != "long":
                wanted[order[-1 - i]] = -1.0
        if p["volatility"]:
            inverse = {}
            for symbol, side in wanted.items():
                if side:
                    sigma = volatility(closes[symbol], p["volatility"])[-1]
                    if sigma > 0:
                        inverse[symbol] = 1 / sigma
            total = sum(inverse.values())
            wanted = {symbol: side * (len(inverse) * inverse[symbol] / total if symbol in inverse else 0)
                      for symbol, side in wanted.items()}
        ctx.weights({symbol: float(Decimal(str(side * p["weight"])).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
                     for symbol, side in wanted.items()})
