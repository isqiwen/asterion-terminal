"""A moving-average rule on one contract or dominant series."""
from asterion_strategy import Strategy

from strategies._rules import lots, units


class MaCross(Strategy):
    """Long while the mean of the last `fast` closes is above that of the last
    `slow`, short while it is below, flat when they are equal."""
    params = {"fast": 5, "slow": 20, "lots": 1, "sides": "both"}

    def ready(self, ctx):
        need = self.params["slow"]
        return all(len(ctx.history(symbol, "close", need)) == need for symbol in ctx.symbols)

    def on_bar(self, ctx, bar):
        fast, slow = self.params["fast"], self.params["slow"]
        if not 0 < fast < slow:
            raise ValueError("a moving average needs 0 < fast < slow")
        closes = units(ctx.history(bar.symbol, "close", slow))
        if len(closes) < slow:
            return
        # The means compared exactly: sum(fast) / fast against sum(slow) / slow.
        compared = sum(closes[-fast:]) * slow - sum(closes) * fast
        ctx.target(bar.symbol, lots((compared > 0) - (compared < 0), self.params))
