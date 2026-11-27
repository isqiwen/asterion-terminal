"""Dynamic-hedge statistical arbitrage across a pair of futures symbols."""
from decimal import Decimal, ROUND_DOWN

import numpy as np

from asterion_strategy import WEIGHT_STEP, kalman_hedge, ou_half_life

from strategies._portfolio import AlignedPortfolio


# Kalman (1960), "A New Approach to Linear Filtering and Prediction Problems", J. Basic Eng. 82(1), 35-45: https://doi.org/10.1115/1.3662552
# Vidyamurthy (2004), "Pairs Trading: Quantitative Methods and Analysis", Wiley, Ch. 5-8.
# Chan (2013), "Algorithmic Trading: Winning Strategies and Their Rationale", Wiley, Ch. 3.
# Adaptation: 2-state Kalman dynamic regression on start-anchored log futures closes (`log P_y = beta_t * log P_x + alpha_t + e_t`),
# causal trailing spread Z-score, optional OU half-life entry gate and beta-hedged weights within gross_weight.
class KalmanPairs(AlignedPortfolio):
    """Dynamic-hedge pairs trading on two futures symbols using a 2-state Kalman filter."""
    params = {
        "window": 40,
        "rebalance": 1,
        "delta": 1e-4,
        "obs_var": 1e-3,
        "entry_z": 2.0,
        "exit_z": 0.5,
        "stop_z": 4.0,
        "max_half_life": 0.0,
        "gross_weight": 0.6,
    }

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["window"] < 5 or not 0 < p["delta"] < 1 or p["obs_var"] <= 0 \
                or not 0 <= p["exit_z"] < p["entry_z"] \
                or (p["stop_z"] != 0 and p["stop_z"] <= p["entry_z"]) \
                or p["max_half_life"] < 0 or p["gross_weight"] <= 0:
            raise ValueError("invalid Kalman pairs parameters")
        self.need = 2 * p["window"]
        self.side = 0

    def on_bars(self, ctx, bars):
        if len(ctx.symbols) != 2:
            raise ValueError("KalmanPairs requires exactly two symbols")
        super().on_bars(ctx, bars)

    def allocate(self, data):
        p = self.params
        y, x = tuple(data.keys())
        cy, cx = data[y]["close"], data[x]["close"]
        if not (np.all(np.isfinite(cy)) and np.all(cy > 0) and np.all(np.isfinite(cx)) and np.all(cx > 0)):
            self.side = 0
            return {y: 0.0, x: 0.0}
        # Anchor log prices to the window start so multiplicative dominant-contract rolls cancel exactly.
        ly = np.log(cy) - np.log(cy[0])
        lx = np.log(cx) - np.log(cx[0])
        betas, _, spread = kalman_hedge(ly, lx, delta=p["delta"], obs_var=p["obs_var"])
        hedge = float(betas[-1])
        # Score the current innovation against the prior window so a blowout cannot mask stop_z.
        base = spread[-p["window"] - 1 : -1]
        if np.all(np.isfinite(base)) and np.isfinite(spread[-1]):
            sigma = float(np.std(base, ddof=1))
            z = (float(spread[-1]) - float(np.mean(base))) / sigma if sigma > 0.0 else 0.0
        else:
            z = np.nan
        side = self.side
        if not np.isfinite(z) or not np.isfinite(hedge) or hedge <= 0:
            side = 0
        elif p["stop_z"] > 0 and abs(z) >= p["stop_z"]:
            side = 0
        elif side == 0:
            allow = True
            if p["max_half_life"] > 0:
                hl = float(ou_half_life(spread, p["window"])[-1])
                allow = np.isfinite(hl) and hl <= p["max_half_life"]
            if allow and z <= -p["entry_z"]:
                side = 1
            elif allow and z >= p["entry_z"]:
                side = -1
        elif (side > 0 and z >= -p["exit_z"]) or (side < 0 and z <= p["exit_z"]):
            side = 0
        self.side = side
        if side == 0:
            return {y: 0.0, x: 0.0}
        gross = p["gross_weight"]
        wy = side * gross / (1.0 + hedge)
        wx = -side * gross * hedge / (1.0 + hedge)
        return {
            y: float(Decimal(str(wy)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN)),
            x: float(Decimal(str(wx)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN)),
        }
