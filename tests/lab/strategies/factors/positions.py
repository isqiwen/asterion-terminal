"""Factors of open interest. Each value uses its bar and those before it."""
import numpy as np


def interest_change(open_interest, lookback: int = 5):
    """Relative change of open interest over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(open_interest), np.nan)
    before = open_interest[:-lookback]
    with np.errstate(divide="ignore", invalid="ignore"):
        result[lookback:] = np.where(before > 0, open_interest[lookback:] / before - 1, np.nan)
    return result
