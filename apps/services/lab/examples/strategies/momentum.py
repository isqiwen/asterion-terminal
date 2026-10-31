"""Time-series momentum on one contract or dominant series."""
from asterion_lab.strategy import Strategy

from strategies._rules import lots


class Momentum(Strategy):
    """Long while the close is above the close `lookback` bars earlier, short
    while it is below."""
    params = {"lookback": 20, "lots": 1, "sides": "both"}

    def on_bar(self, ctx, bar):
        lookback = self.params["lookback"]
        if lookback < 1:
            raise ValueError("momentum needs a lookback of one bar or more")
        closes = ctx.history(bar.symbol, "close", lookback + 1)
        if len(closes) <= lookback:
            return
        side = int(closes[-1] > closes[0]) - int(closes[-1] < closes[0])
        ctx.target(bar.symbol, lots(side, self.params))
