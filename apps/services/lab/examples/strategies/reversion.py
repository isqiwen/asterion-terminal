"""Band reversion on one contract or dominant series."""
import math

from asterion_lab.strategy import Strategy

from strategies._rules import lots, units


class Reversion(Strategy):
    """Short from a close more than `width` standard deviations above the mean
    of the last `window` closes, long from one as far below it; the side is
    given up when a close returns to the mean."""
    params = {"window": 20, "width": 2.0, "lots": 1, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        self.side = {}

    def on_bar(self, ctx, bar):
        window, width = self.params["window"], self.params["width"]
        if window < 2 or width <= 0:
            raise ValueError("a band needs a window of two bars or more and a positive width")
        closes = units(ctx.history(bar.symbol, "close", window))
        if len(closes) < window:
            return
        # Statistics of the window relative to its first close, so that large
        # prices keep their small changes.
        values = [float(close - closes[0]) for close in closes]
        mean = sum(values) / window
        deviation = math.sqrt(max(sum(value * value for value in values) / window - mean * mean, 0.0))
        distance = values[-1] - mean
        side = self.side.get(bar.symbol, 0)
        if distance > width * deviation:
            side = -1
        elif distance < -width * deviation:
            side = 1
        elif (side > 0 and distance >= 0) or (side < 0 and distance <= 0):
            side = 0
        self.side[bar.symbol] = side
        ctx.target(bar.symbol, lots(side, self.params))
