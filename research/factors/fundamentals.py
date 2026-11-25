"""Point-in-time futures fundamental and exchange member-position factors."""
import numpy as np

from asterion_strategy import ts_delta, ts_mean, ts_pct_change
from factors._rolling import ratio as _ratio


def receipt_change(receipt_volume, lookback: int = 20):
    """Relative change in registered warehouse receipts over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    values = np.asarray(receipt_volume, dtype=float)
    positive = np.where(np.isfinite(values) & (values > 0), values, np.nan)
    return ts_pct_change(positive, lookback)


def receipt_to_oi(receipt_volume, open_interest, window: int = 5):
    """Mean ratio of registered warehouse receipts to contract open interest over `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    return ts_mean(_ratio(receipt_volume, open_interest), window)


def member_net_ratio(member_long, member_short, window: int = 5):
    """Mean top-member net long-short imbalance `(long - short) / (long + short)` over `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    longs = np.asarray(member_long, dtype=float)
    shorts = np.asarray(member_short, dtype=float)
    imbalance = _ratio(longs - shorts, longs + shorts)
    return ts_mean(imbalance, window)


def member_net_change(member_long, member_short, lookback: int = 10):
    """Change in top-member net long-short imbalance over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    longs = np.asarray(member_long, dtype=float)
    shorts = np.asarray(member_short, dtype=float)
    imbalance = _ratio(longs - shorts, longs + shorts)
    return ts_delta(imbalance, lookback)


def inventory_change(inventory_total, lookback: int = 20):
    """Relative change in weekly total exchange inventory over `lookback` bars; needs lookback + 1 bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    values = np.asarray(inventory_total, dtype=float)
    positive = np.where(np.isfinite(values) & (values > 0), values, np.nan)
    return ts_pct_change(positive, lookback)
