"""Factors of open interest. Each value uses its bar and those before it."""
import numpy as np

from asterion_strategy import scale_invariant as _scale_invariant, ts_mean as _ts_mean

from factors._common import returns as _returns


# Hong & Yogo (2012), "What Does Futures Market Interest Tell Us about the Macroeconomy and Asset Prices?", JFE 105(3), 473-490: https://doi.org/10.1016/j.jfineco.2012.04.005
# Adaptation: single-symbol open-interest relative growth over `lookback` bars rather than aggregate market open interest.
def interest_change(open_interest, lookback: int = 5):
    """Relative change of open interest over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(open_interest), np.nan)
    before = open_interest[:-lookback]
    with np.errstate(divide="ignore", invalid="ignore"):
        result[lookback:] = np.where(before > 0, open_interest[lookback:] / before - 1, np.nan)
    return result


# Bessembinder & Seguin (1993), "Price Volatility, Trading Volume, and Market Depth: Evidence from Futures Markets", JFQA 28(1), 21-39: https://doi.org/10.2307/2331149
# Adaptation: rolling mean of return-signed open-interest growth; positive values indicate trend-confirming position build-up.
@_scale_invariant
def oi_price_confirmation(close, open_interest, window: int = 20):
    """Mean return-signed one-bar open-interest growth over `window` bars; needs window + 1 bars."""
    ret = _returns(close)
    oi_growth = interest_change(open_interest, 1)
    signed = np.where(np.isfinite(ret) & np.isfinite(oi_growth), np.sign(ret) * oi_growth, np.nan)
    return _ts_mean(signed, window)
