"""A channel breakout on one contract or dominant series."""
from asterion_lab.strategy import Strategy

from strategies._rules import lots


class Breakout(Strategy):
    """Long from a close above the highest high of the `entry` bars before it,
    short from one below their lowest low; the side is given up when a close
    passes the opposite extreme of the `exit` bars before it."""
    params = {"entry": 20, "exit": 10, "lots": 1, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        self.side = {}

    def on_bar(self, ctx, bar):
        entry, leave = self.params["entry"], self.params["exit"]
        if not 0 < leave <= entry:
            raise ValueError("a breakout needs 0 < exit <= entry")
        highs = ctx.history(bar.symbol, "high", entry + 1)[:-1]
        low = ctx.history(bar.symbol, "low", entry + 1)[:-1]
        if len(highs) < entry:
            return
        side = self.side.get(bar.symbol, 0)
        if bar.close > highs.max():
            side = 1
        elif bar.close < low.min():
            side = -1
        elif (side > 0 and bar.close < low[-leave:].min()) or \
                (side < 0 and bar.close > highs[-leave:].max()):
            side = 0
        self.side[bar.symbol] = side
        ctx.target(bar.symbol, lots(side, self.params))
