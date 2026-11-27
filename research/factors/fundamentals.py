"""Point-in-time futures fundamental and exchange member-position factors."""
import numpy as np

from asterion_strategy import ts_delta, ts_mean, ts_pct_change, ts_zscore
from factors._common import ratio as _ratio


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Adaptation: PIT-aligned China exchange registered warehouse receipts instead of aggregate physical inventory indices.
def receipt_change(receipt_volume, lookback: int = 20):
    """Relative change in registered warehouse receipts over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    values = np.asarray(receipt_volume, dtype=float)
    positive = np.where(np.isfinite(values) & (values > 0), values, np.nan)
    return ts_pct_change(positive, lookback)


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Adaptation: registered warehouse receipts scaled by open interest as a deliverable-supply ratio.
def receipt_to_oi(receipt_volume, open_interest, window: int = 5):
    """Mean ratio of registered warehouse receipts to contract open interest over `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    return ts_mean(_ratio(receipt_volume, open_interest), window)


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Adaptation: trailing Z-score of PIT registered warehouse receipts; negative values indicate scarce deliverable supply.
def receipt_zscore(receipt_volume, window: int = 60):
    """Trailing Z-score of registered warehouse receipts over `window` bars; needs window bars."""
    values = np.asarray(receipt_volume, dtype=float)
    valid = np.where(np.isfinite(values) & (values >= 0), values, np.nan)
    return ts_zscore(valid, window)


# Kang, Rouwenhorst & Tang (2020), "A Tale of Two Premiums: The Role of Hedgers and Speculators in Commodity Futures Markets", JF 75(1), 377-417: https://doi.org/10.1111/jofi.12845
# Adaptation: PIT top-member net long-short imbalance from China exchange daily member tables instead of CFTC commercial/non-commercial hedging pressure.
def member_net_ratio(member_long, member_short, window: int = 5):
    """Mean top-member net long-short imbalance `(long - short) / (long + short)` over `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    longs = np.asarray(member_long, dtype=float)
    shorts = np.asarray(member_short, dtype=float)
    imbalance = _ratio(longs - shorts, longs + shorts)
    return ts_mean(imbalance, window)


# Kang, Rouwenhorst & Tang (2020), "A Tale of Two Premiums: The Role of Hedgers and Speculators in Commodity Futures Markets", JF 75(1), 377-417: https://doi.org/10.1111/jofi.12845
# Adaptation: change in PIT top-member net long-short imbalance over `lookback` bars.
def member_net_change(member_long, member_short, lookback: int = 10):
    """Change in top-member net long-short imbalance over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    longs = np.asarray(member_long, dtype=float)
    shorts = np.asarray(member_short, dtype=float)
    imbalance = _ratio(longs - shorts, longs + shorts)
    return ts_delta(imbalance, lookback)


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Adaptation: PIT weekly total exchange inventory relative growth over `lookback` bars.
def inventory_change(inventory_total, lookback: int = 20):
    """Relative change in weekly total exchange inventory over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    values = np.asarray(inventory_total, dtype=float)
    positive = np.where(np.isfinite(values) & (values > 0), values, np.nan)
    return ts_pct_change(positive, lookback)


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Adaptation: trailing Z-score of PIT weekly total exchange inventory; negative values indicate scarce physical supply.
def inventory_zscore(inventory_total, window: int = 60):
    """Trailing Z-score of weekly total exchange inventory over `window` bars; needs window bars."""
    values = np.asarray(inventory_total, dtype=float)
    valid = np.where(np.isfinite(values) & (values >= 0), values, np.nan)
    return ts_zscore(valid, window)
