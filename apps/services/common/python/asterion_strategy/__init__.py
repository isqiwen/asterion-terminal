"""What a strategy is written against.

A strategy is a class in the user's own strategy repository. It reads completed
bars and says what it wants to hold from the next bar on; it never sends
orders. The engine turns what it wants into whole lots and orders, applies the
risk rules and keeps every amount in fixed point.

    class MaCross(Strategy):
        \"\"\"Long above the slow average, short below it.\"\"\"
        params = {"fast": 5, "slow": 20, "lots": 1}

        def ready(self, ctx):
            return all(len(ctx.history(s, "close", self.params["slow"])) == self.params["slow"]
                       for s in ctx.symbols)

        def on_bar(self, ctx, bar):
            closes = ctx.history(bar.symbol, "close", self.params["slow"])
            if len(closes) < self.params["slow"]:
                return
            up = closes[-self.params["fast"]:].mean() > closes.mean()
            ctx.target(bar.symbol, self.params["lots"] if up else -self.params["lots"])

A strategy uses a factor of the repository's factors/ by importing it:
ctx.factor(momentum, bar.symbol, 21, lookback=20) is its value at this bar.

A rule over several symbols overrides on_bars instead, which sees every bar
of one time together, and may hold weights rather than lots.

A product's dominant series is adjusted as of each bar: when it rolls into a
new month, the prices already in its history are scaled by new over old
settlement on the day before, and the new month keeps its own prices. Later
bars never change what an earlier bar saw. State a strategy keeps outside
ctx.history is not scaled; a change of bar.contract marks the roll.
"""
from .combined import CombinedStrategy
from .core import (
    FIELDS,
    FIELDS_SET,
    KINDS,
    PRICES,
    WEIGHT_STEP,
    Bar,
    Column,
    Context,
    Strategy,
    check_factor,
    observe,
    quantize_weight,
    scale_invariant,
    step,
    verify_factor_causality,
)
from .cross_section import (
    PRODUCT_SECTORS,
    cross_section_rank,
    cross_section_zscore,
    liquid_symbols,
    orthogonalize,
    product_sector,
    sector_neutralize,
    winsorize_mad,
)
from .operators import (
    corwin_schultz_spread,
    decay_linear,
    garman_klass_vol,
    kalman_filter,
    kalman_hedge,
    ou_half_life,
    parkinson_vol,
    ts_autocorr,
    ts_corr,
    ts_delta,
    ts_ema,
    ts_ewma_vol,
    ts_expected_shortfall,
    ts_hurst,
    ts_kurt,
    ts_max,
    ts_mean,
    ts_min,
    ts_ols_beta,
    ts_ols_r2,
    ts_ols_resid,
    ts_pct_change,
    ts_rank,
    ts_skew,
    ts_std,
    ts_variance_ratio,
    ts_vwap_dev,
    ts_zscore,
    yang_zhang_vol,
)
from .portfolio import (
    cap_sector_weights,
    combine_weights,
    drawdown_scale,
    erc_weights,
    hrp_weights,
    ledoit_wolf_cov,
    mvo_weights,
    smooth_weights,
    vol_target_weights,
)

__all__ = [
    "FIELDS",
    "FIELDS_SET",
    "PRICES",
    "KINDS",
    "WEIGHT_STEP",
    "PRODUCT_SECTORS",
    "Bar",
    "Column",
    "Context",
    "Strategy",
    "CombinedStrategy",
    "quantize_weight",
    "check_factor",
    "scale_invariant",
    "verify_factor_causality",
    "observe",
    "step",
    "product_sector",
    "ts_mean",
    "ts_max",
    "ts_min",
    "ts_std",
    "ts_delta",
    "ts_pct_change",
    "ts_rank",
    "ts_zscore",
    "ts_corr",
    "decay_linear",
    "ts_ewma_vol",
    "ts_ema",
    "ts_expected_shortfall",
    "ts_ols_beta",
    "ts_ols_resid",
    "ts_ols_r2",
    "kalman_filter",
    "kalman_hedge",
    "ts_autocorr",
    "ts_skew",
    "ts_kurt",
    "ts_variance_ratio",
    "ts_hurst",
    "ou_half_life",
    "parkinson_vol",
    "garman_klass_vol",
    "yang_zhang_vol",
    "corwin_schultz_spread",
    "ts_vwap_dev",
    "cross_section_zscore",
    "cross_section_rank",
    "winsorize_mad",
    "sector_neutralize",
    "orthogonalize",
    "liquid_symbols",
    "vol_target_weights",
    "cap_sector_weights",
    "smooth_weights",
    "drawdown_scale",
    "combine_weights",
    "ledoit_wolf_cov",
    "hrp_weights",
    "erc_weights",
    "mvo_weights",
]
