"""Factors of a symbol's own prices. Each value uses its bar and those before it."""
import numpy as np

from factors._rolling import apply as _apply, ratio as _ratio, returns as _returns


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


def skip_momentum(close, lookback: int = 252, skip: int = 21):
    """Return from lookback bars ago to skip bars ago; needs lookback + 1 bars."""
    if not 0 <= skip < lookback:
        raise ValueError("momentum needs 0 <= skip < lookback")
    result = np.full(len(close), np.nan)
    end = len(close) - skip if skip else len(close)
    with np.errstate(divide="ignore", invalid="ignore"):
        result[lookback:] = close[lookback - skip:end] / close[:-lookback] - 1
    result[~np.isfinite(result)] = np.nan
    return result


def distance_to_high(close, window: int = 252):
    """Close / highest close in the window - 1; needs window bars."""
    return _ratio(close, _apply(close, window, lambda x: x.max(axis=1))) - 1


def momentum_strength(close, window: int = 60):
    """Mean simple return / standard error (a return t-statistic); needs window + 1 bars."""
    changes = _returns(close)
    mean = _apply(changes, window, lambda x: x.mean(axis=1), 2)
    error = _apply(changes, window, lambda x: x.std(axis=1, ddof=1), 2) / np.sqrt(window)
    return _ratio(mean, error)


def blended_momentum(close, fast: int = 21, slow: int = 252, fast_share: float = 0.5):
    """Goulding-Harvey-Mazzoleni (2023) slow/fast sign blend; needs slow + 1 bars."""
    if not 0 < fast < slow or not 0 <= fast_share <= 1:
        raise ValueError("blend needs 0 < fast < slow and fast_share in [0, 1]")
    changes = _returns(close)
    def signal(window):
        mean = _apply(changes, window, lambda x: x.mean(axis=1))
        return np.where(np.isfinite(mean), np.where(mean >= 0, 1.0, -1.0), np.nan)
    return (1 - fast_share) * signal(slow) + fast_share * signal(fast)
