"""Cross-sectional multi-factor ranking and multi-sub-strategy weight synthesis."""
from asterion_strategy import (
    CombinedStrategy,
    Strategy,
    cross_section_rank,
    cross_section_zscore,
    liquid_symbols,
)

from factors.positions import interest_change
from factors.price import momentum, reversal, volatility


class MultiFactorRank(Strategy):
    """Cross-sectional composite of momentum, short-term reversal and open-interest
    change over liquid symbols, inverse-volatility weighted across top/bottom tails."""
    params = {
        "fast": 5,
        "slow": 20,
        "rebalance": 5,
        "count": 3,
        "gross_weight": 0.6,
        "min_volume": 0.0,
        "min_open_interest": 0.0,
        "standardize": "zscore",
    }

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["fast"] < 1 or p["slow"] <= p["fast"] or p["rebalance"] < 1 \
                or p["count"] < 1 or p["gross_weight"] <= 0 \
                or p["standardize"] not in ("zscore", "rank"):
            raise ValueError("invalid multi-factor parameters")
        self.need = p["slow"] + 1
        self.steps = 0

    def ready(self, ctx):
        return any(len(ctx.history(s, "close", self.need)) == self.need for s in ctx.symbols)

    def on_bars(self, ctx, bars):
        p = self.params
        eligible = liquid_symbols(
            ctx,
            bars,
            min_bars=self.need,
            min_volume=p["min_volume"],
            min_open_interest=p["min_open_interest"],
            window=p["slow"],
        )
        if len(eligible) < 2 * p["count"]:
            return
        self.steps += 1
        if (self.steps - 1) % p["rebalance"] != 0:
            return
        norm = cross_section_zscore if p["standardize"] == "zscore" else cross_section_rank
        mom = norm({s: ctx.factor(momentum, s, self.need, lookback=p["slow"]) for s in eligible})
        rev = norm({s: ctx.factor(reversal, s, p["fast"] + 1, lookback=p["fast"]) for s in eligible})
        oi = norm({s: ctx.factor(interest_change, s, p["fast"] + 1, lookback=p["fast"]) for s in eligible})
        composite = {
            s: 0.5 * mom.get(s, 0.0) + 0.25 * rev.get(s, 0.0) + 0.25 * oi.get(s, 0.0)
            for s in eligible
        }
        ordered = sorted(eligible, key=lambda s: composite[s], reverse=True)
        longs = ordered[:p["count"]]
        shorts = ordered[-p["count"]:]
        inv_vol = {}
        for s in longs + shorts:
            vol = ctx.factor(volatility, s, self.need, window=p["slow"])
            inv_vol[s] = 1.0 / vol if vol and vol > 0 else 1.0
        long_sum = sum(inv_vol[s] for s in longs)
        short_sum = sum(inv_vol[s] for s in shorts)
        half = 0.5 * p["gross_weight"]
        target = {s: 0.0 for s in ctx.symbols}
        for s in longs:
            target[s] = half * inv_vol[s] / long_sum
        for s in shorts:
            target[s] = -half * inv_vol[s] / short_sum
        ctx.weights(target)


class CompositePortfolio(CombinedStrategy):
    """Combines two cross-sectional sub-strategies with distinct horizons and rebalance
    cadences into a single portfolio weight stream."""
    params = {
        "trend_lookback": 20,
        "carry_lookback": 10,
        "count": 2,
        "trend_share": 0.6,
        "carry_share": 0.4,
        "gross_weight": 0.6,
        "min_volume": 0.0,
    }

    def components(self):
        p = self.params
        if p["trend_share"] + p["carry_share"] <= 0:
            raise ValueError("at least one component share must be positive")
        trend = MultiFactorRank(
            fast=max(1, p["trend_lookback"] // 4),
            slow=p["trend_lookback"],
            rebalance=5,
            count=p["count"],
            gross_weight=p["gross_weight"],
            min_volume=p["min_volume"],
            standardize="zscore",
        )
        carry = MultiFactorRank(
            fast=max(1, p["carry_lookback"] // 2),
            slow=p["carry_lookback"],
            rebalance=3,
            count=p["count"],
            gross_weight=p["gross_weight"],
            min_volume=p["min_volume"],
            standardize="rank",
        )
        return [(trend, p["trend_share"]), (carry, p["carry_share"])]
