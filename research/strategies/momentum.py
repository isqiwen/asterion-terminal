"""Time-series momentum on one contract or dominant series."""
from asterion_strategy import Strategy

from strategies._common import lots


# Moskowitz, Ooi & Pedersen (2012), "Time Series Momentum": https://doi.org/10.1016/j.jfineco.2011.11.003
# Adaptation: cumulative futures-price return mapped to fixed lots.
class Momentum(Strategy):
    """Long while the close is above the close `lookback` bars earlier, short
    while it is below."""
    params = {"lookback": 20, "lots": 1, "sides": "both"}

    def ready(self, ctx):
        need = self.params["lookback"] + 1
        return all(len(ctx.history(symbol, "close", need)) == need for symbol in ctx.symbols)

    def on_bar(self, ctx, bar):
        lookback = self.params["lookback"]
        if lookback < 1:
            raise ValueError("momentum needs a lookback of one bar or more")
        closes = ctx.history(bar.symbol, "close", lookback + 1)
        if len(closes) <= lookback:
            return
        side = int(closes[-1] > closes[0]) - int(closes[-1] < closes[0])
        ctx.target(bar.symbol, lots(side, self.params))
