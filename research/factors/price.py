"""Factors of a symbol's own prices. Each value uses its bar and those before it."""
import numpy as np

from factors._rolling import apply as _apply, log as _log, ratio as _ratio, returns as _returns


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
    return _apply(_returns(close), window, lambda x: x.std(axis=1, ddof=1), 2)


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


# Da, Gurun & Warachka (2014), "Frog in the Pan: Continuous Information and Momentum", Eq. (1): https://doi.org/10.1093/rfs/hhu003
# Adaptation: a configurable bar window, with zero-return bars retained in the denominator.
def information_discreteness(close, window: int = 126):
    """Return sign times (negative fraction - positive fraction); needs window + 1 bars."""
    balance = _apply(_returns(close), window, lambda x: ((x < 0).sum(axis=1) - (x > 0).sum(axis=1)) / window)
    cumulative = np.full(len(close), np.nan)
    cumulative[window:] = _ratio(close[window:], close[:-window]) - 1
    return np.sign(cumulative) * balance


def return_autocorrelation(close, window: int = 20, lag: int = 1):
    """Pearson correlation of lagged simple returns inside the window; needs window + 1 bars."""
    if not 1 <= lag <= window - 2:
        raise ValueError("autocorrelation needs 1 <= lag <= window - 2")
    def correlation(x):
        first, second = x[:, :-lag], x[:, lag:]
        first = first - first.mean(axis=1, keepdims=True)
        second = second - second.mean(axis=1, keepdims=True)
        return _ratio((first * second).sum(axis=1),
                      np.sqrt((first * first).sum(axis=1) * (second * second).sum(axis=1)))
    return _apply(_returns(close), window, correlation)


# Lo & MacKinlay (1988), "Stock Market Prices Do Not Follow Random Walks: Evidence from a Simple Specification Test", Eqs. (12)-(13): https://doi.org/10.1093/rfs/1.1.41
# Adaptation: return 1 + M_r(q), the ratio of the corrected overlapping variance estimates.
def variance_ratio(close, window: int = 20, lag: int = 2):
    """Lo-MacKinlay variance ratio at lag q; needs window + 1 bars; constant returns are missing."""
    if not 2 <= lag < window:
        raise ValueError("variance ratio needs 2 <= lag < window")
    def estimate(x):
        centered = x - x.mean(axis=1, keepdims=True)
        sums = np.lib.stride_tricks.sliding_window_view(centered, lag, axis=1).sum(axis=2)
        long_variance = (sums * sums).sum(axis=1) / (lag * (window - lag + 1) * (1 - lag / window))
        return _ratio(long_variance, (centered * centered).sum(axis=1) / (window - 1))
    return _apply(np.diff(_log(close), prepend=np.nan), window, estimate)


# Goulding, Harvey & Mazzoleni (2023), "Momentum Turning Points", Eq. (11): https://doi.org/10.1016/j.jfineco.2023.05.007
# Adaptation: static blending of futures-bar mean-return signs with configurable windows.
def blended_momentum(close, fast: int = 21, slow: int = 252, fast_share: float = 0.5):
    """Goulding-Harvey-Mazzoleni (2023) slow/fast sign blend; needs slow + 1 bars."""
    if not 0 < fast < slow or not 0 <= fast_share <= 1:
        raise ValueError("blend needs 0 < fast < slow and fast_share in [0, 1]")
    changes = _returns(close)
    def signal(window):
        mean = _apply(changes, window, lambda x: x.mean(axis=1))
        return np.where(np.isfinite(mean), np.where(mean >= 0, 1.0, -1.0), np.nan)
    return (1 - fast_share) * signal(slow) + fast_share * signal(fast)
