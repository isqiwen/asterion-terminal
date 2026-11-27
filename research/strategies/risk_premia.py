"""Cross-sectional futures premia, using only completed shared observation times."""
from decimal import Decimal

import numpy as np

from asterion_strategy import WEIGHT_STEP, cap_sector_weights, cross_section_rank, sector_neutralize

from factors.positions import interest_change
from factors.price import information_discreteness, momentum, volatility
from factors.risk import downside_volatility, maximum_return, realized_skewness
from factors.term_structure import basis_momentum, roll_yield_carry
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


# Koijen et al. (2018), "Carry": https://doi.org/10.1016/j.jfineco.2017.11.002
# Adaptation: dominant/next-dominant log carry, configurable tails and inverse-volatility side weights.
class CarryRank(_Ranked):
    """Long high annualized roll yield, short low carry; inverse-volatility weights per tail.

    Futures adaptation of Koijen et al. (JFE 2018), using dominant/next-dominant carry.
    """
    params = {**_Ranked.params, "window": 5}
    factor_extra_bar = 0

    def scores(self, data):
        return {s: roll_yield_carry(row["roll_yield"], self.params["window"])[-1]
                for s, row in data.items()}


# Boons & Prado (2019), "Basis-Momentum", Journal of Finance 74(1), 239-279, Eq. (2): https://doi.org/10.1111/jofi.12738
# Adaptation: cross-sectional tails on dominant vs next-dominant same-contract return spread, inverse-volatility weighted.
class BasisMomentumRank(_Ranked):
    """Long high basis-momentum futures, short low basis-momentum futures (Boons & Prado, JF 2019)."""
    params = {**_Ranked.params, "window": 20}
    factor_extra_bar = 0

    def scores(self, data):
        return {s: basis_momentum(row["near_return"], row["far_return"], self.params["window"])[-1]
                for s, row in data.items()}


# Fernandez-Perez et al. (2018), "The Skewness of Commodity Futures Returns": https://doi.org/10.1016/j.jbankfin.2017.06.015
# Adaptation: configurable return window and tail count, with inverse-volatility side weights.
class SkewnessRank(_Ranked):
    """Long negative-skew futures, short positive-skew futures (Fernandez-Perez et al., 2018)."""
    params = {**_Ranked.params, "window": 126}
    minimum_window = 3

    def scores(self, data):
        return {s: -realized_skewness(row["close"], self.params["window"])[-1]
                for s, row in data.items()}


# Ang, Hodrick, Xing & Zhang (2006), "The Cross-Section of Volatility and Expected Returns", JF 61(1), 259-299: https://doi.org/10.1111/j.1540-6261.2006.00836.x
# Adaptation: total trailing return volatility across futures with equal long/short risk-scaled budgets.
class LowVolatilityRank(_Ranked):
    """Long low-volatility futures and short high-volatility futures, with equal side budgets."""
    minimum_window = 2

    def scores(self, data):
        return {s: -volatility(row["close"], self.params["window"])[-1] for s, row in data.items()}


# Ang, Chen & Xing (2006), "Downside Risk", RFS 19(4), 1191-1239: https://doi.org/10.1093/rfs/hhj035
# Adaptation: trailing realized downside semideviation across futures with equal long/short budgets.
class DownsideRiskRank(_Ranked):
    """Long low downside semideviation, short high downside semideviation."""

    def scores(self, data):
        return {s: -downside_volatility(row["close"], self.params["window"])[-1]
                for s, row in data.items()}


# Hong & Yogo (2012), "What Does Futures Market Interest Tell Us about the Macroeconomy and Asset Prices?", JFE 105(3), 473-490: https://doi.org/10.1016/j.jfineco.2012.04.005
# Adaptation: cross-sectional ranking on single-symbol open-interest growth rather than aggregate open interest.
class OpenInterestRank(_Ranked):
    """Long rising open interest, short falling open interest; this is not hedging pressure."""

    def scores(self, data):
        return {s: interest_change(row["open_interest"], self.params["window"])[-1]
                for s, row in data.items()}


# Antonacci (2014), "Dual Momentum Investing"; Moskowitz, Ooi & Pedersen (2012): https://doi.org/10.1016/j.jfineco.2011.11.003
# Adaptation: cross-sectional momentum tails gated by absolute return sign, leaving unused budget in cash.
class DualMomentum(_Ranked):
    """Rank past returns; take longs only above zero and shorts only below zero, leaving unused cash."""
    params = {**_Ranked.params, "window": 126}
    absolute = True

    def scores(self, data):
        return {s: momentum(row["close"], self.params["window"])[-1] for s, row in data.items()}


# Da, Gurun & Warachka (2014), "Frog in the Pan: Continuous Information and Momentum", Eq. (1): https://doi.org/10.1093/rfs/hhu003
# Adaptation: filter futures by ID threshold before momentum ranking instead of the paper's equity double sort.
class ContinuousMomentumRank(_Ranked):
    """Rank momentum among paths with low information discreteness, with inverse-volatility weights."""
    params = {**_Ranked.params, "window": 126, "max_discreteness": 0.0}

    def __init__(self, **values):
        super().__init__(**values)
        if not -1 <= self.params["max_discreteness"] <= 1:
            raise ValueError("max_discreteness must be in [-1, 1]")

    def scores(self, data):
        p = self.params
        return {s: momentum(row["close"], p["window"])[-1] for s, row in data.items()
                if information_discreteness(row["close"], p["window"])[-1] <= p["max_discreteness"]}


# Bali, Cakici & Whitelaw (2011), "Maxing out: Stocks as lotteries and the cross-section of expected returns": https://doi.org/10.1016/j.jfineco.2010.08.014
# Adaptation: low-MAX/high-MAX futures tails with inverse-volatility weights; the original evidence concerns equities.
class MaximumReturnRank(_Ranked):
    """Long low maximum-return futures, short high maximum-return futures."""
    params = {**_Ranked.params, "window": 20}

    def scores(self, data):
        return {s: -maximum_return(row["close"], self.params["window"])[-1] for s, row in data.items()}


# Carry source: Koijen et al. (2018), "Carry": https://doi.org/10.1016/j.jfineco.2017.11.002
# Project-specific blend of momentum and carry ranks, using dominant/next-dominant log carry.
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


# Asness, Moskowitz & Pedersen (2013), "Value and Momentum Everywhere", JF 68(3), 929-985: https://doi.org/10.1111/jofi.12021
# Fernandez-Perez et al. (2018), "The Skewness of Commodity Futures Returns", JBF 86, 143-158: https://doi.org/10.1016/j.jbankfin.2017.06.015
# Adaptation: blend momentum, negative skewness and optional carry ranks, demeaned inside China futures sectors with sector gross caps.
class SectorNeutralRank(_Ranked):
    """Multi-factor cross-sectional alpha neutralized within China futures sectors with per-sector exposure caps."""
    params = {**_Ranked.params, "window": 60, "use_carry": True, "max_sector_gross": 0.35}
    minimum_window = 3

    def __init__(self, **values):
        super().__init__(**values)
        if self.params["max_sector_gross"] < 0:
            raise ValueError("max_sector_gross must be nonnegative")

    def scores(self, data):
        p = self.params
        w = p["window"]
        raw = {}
        for s, row in data.items():
            parts = [momentum(row["close"], w)[-1], -realized_skewness(row["close"], w)[-1]]
            if p["use_carry"]:
                parts.append(roll_yield_carry(row["roll_yield"], w)[-1])
            if np.isfinite(parts).all():
                raw[s] = parts
        if not raw:
            return {}
        n_parts = len(next(iter(raw.values())))
        ranked = [cross_section_rank({s: vals[i] for s, vals in raw.items()}) for i in range(n_parts)]
        blended = {s: sum(r[s] for r in ranked) / n_parts for s in raw}
        return sector_neutralize(blended)

    def allocate(self, data):
        target = super().allocate(data)
        if self.params["max_sector_gross"] > 0:
            target = cap_sector_weights(target, max_sector_gross=self.params["max_sector_gross"])
        return target
