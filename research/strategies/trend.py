"""Volatility-scaled trend, multi-horizon EWMAC and slow/fast momentum mixtures."""
from decimal import Decimal, ROUND_DOWN

import numpy as np

from asterion_strategy import (
    WEIGHT_STEP,
    AlignedPortfolio,
    cap_sector_weights,
    smooth_weights,
    ts_ema,
    ts_ewma_vol,
    vol_target_weights,
)

from factors.price import blended_momentum, momentum, volatility


def _ewmac_scale(fast: int, slow: int) -> float:
    """Analytical random-walk standard deviation of `EMA(fast) - EMA(slow)` per 1-sigma price move."""
    qf = 1.0 - 2.0 / (fast + 1.0)
    qs = 1.0 - 2.0 / (slow + 1.0)
    return float(np.sqrt(qs * qs / (1.0 - qs * qs) + qf * qf / (1.0 - qf * qf) - 2.0 * qf * qs / (1.0 - qf * qs)))


# Moskowitz, Ooi & Pedersen (2012), "Time Series Momentum": https://doi.org/10.1016/j.jfineco.2011.11.003
# Adaptation: roll-adjusted futures returns, sample volatility and explicit per-symbol/gross caps.
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
        return vol_target_weights(
            signals,
            risks,
            target_vol=p["target_volatility"],
            gross_weight=p["gross_weight"],
            max_weight=p["max_weight"],
        )


# Baz, Granger, Harvey, Le Roux & Rattray (2015), "Dissecting Investment Strategies in the Cross Section and Time Series", Eqs. (4)-(6): https://ssrn.com/abstract=2695101
# Hurst, Ooi & Pedersen (2017), "A Century of Evidence on Trend-Following Investing", JPM 44(1), 15-29: https://doi.org/10.3905/jpm.2017.44.1.015
# Adaptation: three EWMAC horizons (fast/4*fast, 2*fast/8*fast, 4*fast/16*fast) via C++-backed ts_ema/ts_ewma_vol,
# tanh response, volatility targeting, sector gross caps and turnover deadband.
class MultiHorizonTrend(AlignedPortfolio):
    """Multi-horizon volatility-targeted EWMAC CTA across fast, medium and slow trend speeds."""
    params = {
        "fast": 8,
        "volatility": 32,
        "periods": 252,
        "rebalance": 5,
        "target_volatility": 0.12,
        "max_weight": 0.2,
        "gross_weight": 1.0,
        "max_sector_gross": 0.4,
        "deadband": 0.01,
    }

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["fast"] < 2 or p["volatility"] < 2 or p["periods"] < 1 \
                or min(p["target_volatility"], p["max_weight"], p["gross_weight"]) <= 0 \
                or p["max_sector_gross"] < 0 or p["deadband"] < 0:
            raise ValueError("invalid multi-horizon trend parameters")
        f = p["fast"]
        self.horizons = ((f, 4 * f), (2 * f, 8 * f), (4 * f, 16 * f))
        self.scales = tuple(_ewmac_scale(a, b) for a, b in self.horizons)
        self.need = max(16 * f, p["volatility"] + 1)
        self._previous = {}

    def allocate(self, data):
        p = self.params
        signals, risks = {}, {}
        sqrt_periods = float(np.sqrt(p["periods"]))
        for symbol, row in data.items():
            close = row["close"]
            vol_bar = float(ts_ewma_vol(close, p["volatility"], annualize=0.0)[-1])
            if not np.isfinite(vol_bar) or vol_bar <= 0.0 or not np.isfinite(close[-1]) or close[-1] <= 0.0:
                signals[symbol], risks[symbol] = 0.0, np.nan
                continue
            parts = []
            for (fast, slow), scale in zip(self.horizons, self.scales):
                ema_fast = float(ts_ema(close, fast)[-1])
                ema_slow = float(ts_ema(close, slow)[-1])
                if np.isfinite(ema_fast) and np.isfinite(ema_slow):
                    z = (ema_fast - ema_slow) / (close[-1] * vol_bar * scale)
                    parts.append(float(np.tanh(z)))
            signals[symbol] = float(np.mean(parts)) if len(parts) == len(self.horizons) else 0.0
            risks[symbol] = vol_bar * sqrt_periods
        weights = vol_target_weights(
            signals,
            risks,
            target_vol=p["target_volatility"],
            gross_weight=p["gross_weight"],
            max_weight=p["max_weight"],
        )
        if p["deadband"] > 0.0 and self._previous:
            weights = smooth_weights(self._previous, weights, alpha=1.0, deadband=p["deadband"])
        if p["max_sector_gross"] > 0.0:
            weights = cap_sector_weights(weights, max_sector_gross=p["max_sector_gross"])
        gross = sum(abs(v) for v in weights.values())
        if gross > p["gross_weight"]:
            factor = p["gross_weight"] / gross
            weights = {s: float(Decimal(str(v * factor)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
                       for s, v in weights.items()}
        self._previous = dict(weights)
        return weights


# Goulding, Harvey & Mazzoleni (2023), "Momentum Turning Points", Eq. (11): https://doi.org/10.1016/j.jfineco.2023.05.007
# Adaptation: the static fast/slow blend on futures bars, scaled by a fixed per-symbol weight.
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
