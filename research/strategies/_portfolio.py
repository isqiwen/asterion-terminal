"""Shared observation times and allocation rules for built-in portfolios."""
from collections import deque
from decimal import Decimal, ROUND_DOWN

import numpy as np

from asterion_strategy import FIELDS, WEIGHT_STEP, Strategy


class AlignedPortfolio(Strategy):
    def __init__(self, **values):
        super().__init__(**values)
        if self.params["rebalance"] < 1:
            raise ValueError("rebalance must be at least one bar")
        self.indices = None
        self.shared = 0

    def ready(self, ctx):
        return self.shared >= self.need

    def on_bars(self, ctx, bars):
        if len(bars) != len(ctx.symbols):
            return
        if self.indices is None:
            self.indices = {s: deque(maxlen=self.need) for s in ctx.symbols}
        for symbol in ctx.symbols:
            self.indices[symbol].append(len(ctx.history(symbol, "close", 1 << 30)) - 1)
        self.shared += 1
        if self.shared < self.need or (self.shared - self.need) % self.params["rebalance"]:
            return
        # Re-read prices from Context so a dominant-contract roll adjusts older observations.
        data = {s: {field: ctx.history(s, field, 1 << 30)[list(indices)] for field in FIELDS}
                for s, indices in self.indices.items()}
        ctx.weights(self.allocate(data))

    def allocate(self, data):
        raise NotImplementedError


def tails(scores, count, absolute=False):
    ordered = sorted(scores, key=scores.get, reverse=True)
    if len(ordered) < 2 * count or scores[ordered[count - 1]] <= scores[ordered[-count]]:
        return [], []
    longs, shorts = ordered[:count], ordered[-count:]
    if absolute:
        longs = [s for s in longs if scores[s] > 0]
        shorts = [s for s in shorts if scores[s] < 0]
    return longs, shorts


def risk_weights(signals, risks, gross_weight, target_volatility, max_weight):
    weights = {s: float(np.clip(signal * target_volatility / risks[s], -max_weight, max_weight))
               if np.isfinite(signal) and np.isfinite(risks[s]) and risks[s] > 0 else 0.0
               for s, signal in signals.items()}
    gross = sum(abs(value) for value in weights.values())
    scale = min(1.0, gross_weight / gross) if gross else 1.0
    # Round toward zero so wire precision cannot increase the declared gross exposure.
    return {s: float(Decimal(str(value * scale)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
            for s, value in weights.items()}
