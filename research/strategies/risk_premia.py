"""Cross-sectional futures premia, using only completed shared observation times."""
from decimal import Decimal

import numpy as np

from asterion_strategy import WEIGHT_STEP, cross_section_rank

from factors.positions import interest_change
from factors.price import momentum, volatility
from factors.risk import downside_volatility, realized_skewness
from factors.term_structure import roll_yield_carry
from strategies._portfolio import AlignedPortfolio, tails


class _Ranked(AlignedPortfolio):
    params = {"window": 60, "rebalance": 5, "count": 2, "gross_weight": 0.6,
              "volatility": 20, "min_volume": 0.0, "min_open_interest": 0.0}
    minimum_window = 1
    factor_extra_bar = 1
    absolute = False

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["window"] < self.minimum_window or p["count"] < 1 or p["gross_weight"] <= 0 \
                or p["volatility"] < 2 or p["min_volume"] < 0 or p["min_open_interest"] < 0:
            raise ValueError("invalid risk-premium parameters")
        self.need = max(p["window"] + self.factor_extra_bar, p["volatility"] + 1)

    def scores(self, data):
        raise NotImplementedError

    def allocate(self, data):
        p = self.params
        risk = {s: volatility(row["close"], p["volatility"])[-1] for s, row in data.items()}
        eligible = {s: row for s, row in data.items()
                    if np.isfinite(risk[s]) and risk[s] > 0
                    and (p["min_volume"] == 0 or np.mean(row["volume"][-p["window"]:]) >= p["min_volume"])
                    and (p["min_open_interest"] == 0
                         or np.mean(row["open_interest"][-p["window"]:]) >= p["min_open_interest"])}
        scores = {s: float(value) for s, value in self.scores(eligible).items() if np.isfinite(value)}
        longs, shorts = tails(scores, p["count"], self.absolute)
        target = dict.fromkeys(data, 0.0)
        budget = int(Decimal(str(p["gross_weight"])) / (2 * WEIGHT_STEP))
        for symbols, side in ((longs, 1), (shorts, -1)):
            total = sum(1 / risk[s] for s in symbols)
            remaining = budget
            for i, s in enumerate(symbols):
                # Allocate whole wire units; both tails then have exactly the same total.
                units = remaining if i == len(symbols) - 1 else int(budget / risk[s] / total)
                target[s] = side * float(units * WEIGHT_STEP)
                remaining -= units
        return target


class CarryRank(_Ranked):
    """Long high annualized roll yield, short low carry; inverse-volatility weights per tail.

    Futures adaptation of Koijen et al. (JFE 2018), using dominant/next-dominant carry.
    """
    params = {**_Ranked.params, "window": 5}
    factor_extra_bar = 0

    def scores(self, data):
        return {s: roll_yield_carry(row["roll_yield"], self.params["window"])[-1]
                for s, row in data.items()}


class SkewnessRank(_Ranked):
    """Long negative-skew futures, short positive-skew futures (Fernandez-Perez et al., 2018)."""
    params = {**_Ranked.params, "window": 126}
    minimum_window = 3

    def scores(self, data):
        return {s: -realized_skewness(row["close"], self.params["window"])[-1]
                for s, row in data.items()}


class LowVolatilityRank(_Ranked):
    """Long low-volatility futures and short high-volatility futures, with equal side budgets."""
    minimum_window = 2

    def scores(self, data):
        return {s: -volatility(row["close"], self.params["window"])[-1] for s, row in data.items()}


class DownsideRiskRank(_Ranked):
    """Long low downside semideviation, short high downside semideviation."""

    def scores(self, data):
        return {s: -downside_volatility(row["close"], self.params["window"])[-1]
                for s, row in data.items()}


class OpenInterestRank(_Ranked):
    """Long rising open interest, short falling open interest; this is not hedging pressure."""

    def scores(self, data):
        return {s: interest_change(row["open_interest"], self.params["window"])[-1]
                for s, row in data.items()}


class DualMomentum(_Ranked):
    """Rank past returns; take longs only above zero and shorts only below zero, leaving unused cash."""
    params = {**_Ranked.params, "window": 126}
    absolute = True

    def scores(self, data):
        return {s: momentum(row["close"], self.params["window"])[-1] for s, row in data.items()}


class TrendCarryRank(_Ranked):
    """Blend cross-sectional ranks of momentum and actual roll yield, then hold the two tails."""
    params = {**_Ranked.params, "trend_share": 0.5}

    def __init__(self, **values):
        super().__init__(**values)
        if not 0 <= self.params["trend_share"] <= 1:
            raise ValueError("trend_share must be in [0, 1]")

    def scores(self, data):
        p = self.params
        values = {s: (momentum(row["close"], p["window"])[-1],
                      roll_yield_carry(row["roll_yield"], p["window"])[-1]) for s, row in data.items()}
        valid = {s: pair for s, pair in values.items() if np.isfinite(pair).all()}
        trend = cross_section_rank({s: pair[0] for s, pair in valid.items()})
        carry = cross_section_rank({s: pair[1] for s, pair in valid.items()})
        return {s: p["trend_share"] * trend[s] + (1 - p["trend_share"]) * carry[s]
                for s in valid}
