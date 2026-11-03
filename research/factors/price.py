"""Factors of a symbol's own prices. Each value uses its bar and those before it."""
import numpy as np


def momentum(close, lookback: int = 20):
    """Return over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(close), np.nan)
    result[lookback:] = close[lookback:] / close[:-lookback] - 1
    return result


def reversal(close, lookback: int = 5):
    """Return over the last `lookback` bars, negated: the recent losers rank high."""
    return -momentum(close, lookback)


def volatility(close, window: int = 20):
    """Standard deviation of the last `window` one-bar returns."""
    if window < 2:
        raise ValueError("window is at least two bars")
    changes = np.full(len(close), np.nan)
    changes[1:] = close[1:] / close[:-1] - 1
    result = np.full(len(close), np.nan)
    for i in range(window, len(close)):
        result[i] = np.std(changes[i - window + 1:i + 1], ddof=1)
    return result
