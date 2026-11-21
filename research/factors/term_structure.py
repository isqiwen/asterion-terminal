"""Factors of the futures term structure (dominant vs next-dominant roll yield)."""
import numpy as np

from factors._rolling import ratio as _ratio
from factors.price import volatility as _volatility
from factors.technical import price_zscore as _price_zscore


# Koijen et al. (2018), "Carry": https://doi.org/10.1016/j.jfineco.2017.11.002
# Adaptation: annualized log carry between dominant and next-dominant delivery months.
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


def carry_to_risk(close, roll_yield, window: int = 60, periods: int = 252):
    """Mean annualized roll yield / annualized return volatility; needs window + 1 bars."""
    if periods < 1:
        raise ValueError("periods per year must be positive")
    return _ratio(roll_yield_carry(roll_yield, window), _volatility(close, window) * np.sqrt(periods))


def carry_zscore(roll_yield, window: int = 60):
    """Roll yield standardized against its trailing window; needs window bars."""
    return _price_zscore(roll_yield, window)
