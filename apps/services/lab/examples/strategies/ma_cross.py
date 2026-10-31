"""A moving-average rule on one contract or dominant series."""
from asterion_lab.strategy import Strategy


class MaCross(Strategy):
    """Long while the fast average of closes is above the slow one, short below it."""
    params = {"fast": 5, "slow": 20, "lots": 1}

    def on_bar(self, ctx, bar):
        closes = ctx.history(bar.symbol, "close", self.params["slow"])
        if len(closes) < self.params["slow"]:
            return
        up = closes[-self.params["fast"]:].mean() > closes.mean()
        ctx.target(bar.symbol, self.params["lots"] if up else -self.params["lots"])
