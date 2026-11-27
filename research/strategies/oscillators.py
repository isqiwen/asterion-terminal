"""Oscillator reversion and volatility-band breakout rules."""
import numpy as np

from asterion_strategy import SIDES, Strategy, side_lots

from factors.technical import average_true_range, rsi


# Wilder (1978), "New Concepts in Technical Trading Systems", Section VI (RSI).
# Adaptation: Cutler finite-window RSI oversold/overbought reversion exiting at RSI 50.
class RsiReversion(Strategy):
    """Enter below lower / above upper Cutler RSI; exit each side at RSI 50."""
    params = {"window": 14, "lower": 30.0, "upper": 70.0, "lots": 1, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["window"] < 1 or not 0 < p["lower"] < 50 < p["upper"] < 100 \
                or p["lots"] < 1 or p["sides"] not in SIDES:
            raise ValueError("invalid RSI reversion parameters")
        self.side = {}

    def ready(self, ctx):
        need = self.params["window"] + 1
        return all(len(ctx.history(s, "close", need)) == need for s in ctx.symbols)

    def on_bar(self, ctx, bar):
        p = self.params
        value = ctx.factor(rsi, bar.symbol, p["window"] + 1, window=p["window"])
        if not np.isfinite(value):
            return
        side = self.side.get(bar.symbol, 0)
        if value < p["lower"]:
            side = 1
        elif value > p["upper"]:
            side = -1
        elif (side > 0 and value >= 50) or (side < 0 and value <= 50):
            side = 0
        self.side[bar.symbol] = side
        ctx.target(bar.symbol, side_lots(side, p["lots"], p["sides"]))


# Keltner (1960), "How to Make Money in Commodities"; Wilder (1978), "New Concepts in Technical Trading Systems" (ATR).
# Adaptation: breakout outside prior simple mean +/- width * prior simple ATR, exiting on crossing the prior mean.
class AtrBreakout(Strategy):
    """Enter outside the prior mean +/- width * prior simple ATR; exit on crossing that mean."""
    params = {"window": 20, "width": 2.0, "lots": 1, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["window"] < 1 or p["width"] <= 0 or p["lots"] < 1 or p["sides"] not in SIDES:
            raise ValueError("invalid ATR breakout parameters")
        self.side = {}

    def ready(self, ctx):
        need = self.params["window"] + 2
        return all(len(ctx.history(s, "close", need)) == need for s in ctx.symbols)

    def on_bar(self, ctx, bar):
        p = self.params
        values = {field: ctx.history(bar.symbol, field, p["window"] + 2)[:-1]
                  for field in ("high", "low", "close")}
        if len(values["close"]) < p["window"] + 1:
            return
        mean = values["close"][-p["window"]:].mean()
        width = p["width"] * average_true_range(**values, window=p["window"])[-1]
        if not np.isfinite(width):
            return
        side = self.side.get(bar.symbol, 0)
        if bar.close > mean + width:
            side = 1
        elif bar.close < mean - width:
            side = -1
        elif (side > 0 and bar.close <= mean) or (side < 0 and bar.close >= mean):
            side = 0
        self.side[bar.symbol] = side
        ctx.target(bar.symbol, side_lots(side, p["lots"], p["sides"]))
