"""Volatility-scaled trend and slow/fast momentum mixtures."""
import numpy as np

from factors.price import blended_momentum, momentum, volatility
from strategies._portfolio import AlignedPortfolio, risk_weights


class VolatilityTargetMomentum(AlignedPortfolio):
    """Time-series momentum scaled by trailing volatility (Moskowitz-Ooi-Pedersen, 2012).

    target_volatility is the annual risk allocation per symbol, capped by max_weight and gross_weight.
    """
    params = {"lookback": 252, "volatility": 60, "periods": 252, "rebalance": 21,
              "target_volatility": 0.1, "max_weight": 0.2, "gross_weight": 1.0}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["lookback"] < 1 or p["volatility"] < 2 or p["periods"] < 1 \
                or min(p["target_volatility"], p["max_weight"], p["gross_weight"]) <= 0:
            raise ValueError("invalid volatility-target parameters")
        self.need = max(p["lookback"], p["volatility"]) + 1

    def allocate(self, data):
        p = self.params
        signals = {s: np.sign(momentum(row["close"], p["lookback"])[-1]) for s, row in data.items()}
        risks = {s: volatility(row["close"], p["volatility"])[-1] * np.sqrt(p["periods"])
                 for s, row in data.items()}
        return risk_weights(signals, risks, p["gross_weight"], p["target_volatility"], p["max_weight"])


class TurningPointMomentum(AlignedPortfolio):
    """Static slow/fast sign blend from Momentum Turning Points (JFE 2023), adapted to futures bars.

    Equal shares hold flat when the two trends disagree; weight is the absolute budget per symbol.
    """
    params = {"fast": 21, "slow": 252, "fast_share": 0.5, "weight": 0.1, "rebalance": 21}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if not 0 < p["fast"] < p["slow"] or not 0 <= p["fast_share"] <= 1 or p["weight"] <= 0:
            raise ValueError("invalid turning-point parameters")
        self.need = p["slow"] + 1

    def allocate(self, data):
        p = self.params
        values = {s: blended_momentum(row["close"], p["fast"], p["slow"], p["fast_share"])[-1]
                  for s, row in data.items()}
        return {s: float(value * p["weight"]) if np.isfinite(value) else 0.0 for s, value in values.items()}
