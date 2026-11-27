"""Factors of the futures term structure (dominant vs next-dominant roll yield)."""
import numpy as np

from asterion_strategy import scale_invariant as _scale_invariant

from factors._rolling import apply as _apply, ratio as _ratio
from factors.price import volatility as _volatility
from factors.technical import price_zscore as _price_zscore


# Koijen et al. (2018), "Carry": https://doi.org/10.1016/j.jfineco.2017.11.002
# Adaptation: annualized log carry between dominant and next-dominant delivery months.
def roll_yield_carry(roll_yield, window: int = 5):
    """Mean annualized roll yield over the last `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    return _apply(roll_yield, window, lambda x: x.mean(axis=1), 1)


def roll_yield_momentum(roll_yield, lookback: int = 20):
    """Change in annualized roll yield over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(roll_yield), np.nan)
    result[lookback:] = roll_yield[lookback:] - roll_yield[:-lookback]
    return result


@_scale_invariant
def carry_to_risk(close, roll_yield, window: int = 60, periods: int = 252):
    """Mean annualized roll yield / annualized return volatility; needs window + 1 bars."""
    if periods < 1:
        raise ValueError("periods per year must be positive")
    return _ratio(roll_yield_carry(roll_yield, window), _volatility(close, window) * np.sqrt(periods))


def carry_zscore(roll_yield, window: int = 60):
    """Roll yield standardized against its trailing window; needs window bars."""
    return _price_zscore(roll_yield, window)


# Boons & Prado (2019), "Basis-Momentum", Journal of Finance 74(1), 239-279, Eq. (2):
# https://doi.org/10.1111/jofi.12738
# Adaptation: rolling window of exact same-contract dominant vs next-dominant returns (daily or intraday).
def basis_momentum(near_return, far_return, window: int = 20):
    """Compounded near-contract minus far-contract return over the last `window` bars."""
    if window < 1:
        raise ValueError("window is at least one bar")
    near = np.asarray(near_return, dtype=float)
    far = np.asarray(far_return, dtype=float)
    result = np.full(len(near), np.nan)
    if len(near) < window:
        return result
    w_near = np.lib.stride_tricks.sliding_window_view(near, window)
    w_far = np.lib.stride_tricks.sliding_window_view(far, window)
    valid = np.all(np.isfinite(w_near), axis=1) & np.all(np.isfinite(w_far), axis=1)
    safe_near = np.where(valid[:, None], 1.0 + w_near, 1.0)
    safe_far = np.where(valid[:, None], 1.0 + w_far, 1.0)
    diff = np.prod(safe_near, axis=1) - np.prod(safe_far, axis=1)
    result[window - 1:] = np.where(valid & np.isfinite(diff), diff, np.nan)
    return result
