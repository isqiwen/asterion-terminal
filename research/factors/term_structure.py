"""Factors of the futures term structure (dominant vs next-dominant roll yield)."""
import numpy as np


def roll_yield_carry(roll_yield, window: int = 5):
    """Mean annualized roll yield over the last `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    result = np.full(len(roll_yield), np.nan)
    for i in range(window - 1, len(roll_yield)):
        chunk = roll_yield[i - window + 1:i + 1]
        if np.all(np.isfinite(chunk)):
            result[i] = float(np.mean(chunk))
    return result


def roll_yield_momentum(roll_yield, lookback: int = 20):
    """Change in annualized roll yield over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(roll_yield), np.nan)
    result[lookback:] = roll_yield[lookback:] - roll_yield[:-lookback]
    return result
