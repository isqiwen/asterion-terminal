"""Factors of a symbol's own prices. Each value uses its bar and those before it."""
import numpy as np

from asterion_strategy import (
    ou_half_life as _ou_half_life,
    scale_invariant as _scale_invariant,
    ts_hurst as _ts_hurst,
)

from factors._rolling import apply as _apply, log as _log, ratio as _ratio, returns as _returns


@_scale_invariant
def momentum(close, lookback: int = 20):
    """Return over the last `lookback` bars."""
    if lookback < 1:
        raise ValueError("lookback is at least one bar")
    result = np.full(len(close), np.nan)
    result[lookback:] = close[lookback:] / close[:-lookback] - 1
    return result


@_scale_invariant
def reversal(close, lookback: int = 5):
    """Return over the last `lookback` bars, negated: the recent losers rank high."""
    return -momentum(close, lookback)


@_scale_invariant
def volatility(close, window: int = 20):
    """Standard deviation of the last `window` one-bar returns."""
    if window < 2:
        raise ValueError("window is at least two bars")
    return _apply(_returns(close), window, lambda x: x.std(axis=1, ddof=1), 2)


# Jegadeesh & Titman (1993), "Returns to Buying Winners and Selling Losers", JF 48(1), 65-91: https://doi.org/10.1111/j.1540-6261.1993.tb04702.x
# Asness, Moskowitz & Pedersen (2013), "Value and Momentum Everywhere", JF 68(3), 929-985: https://doi.org/10.1111/jofi.12021
# Adaptation: cumulative return from `lookback` to `skip` bars ago on futures bars, skipping the recent reversal leg.
@_scale_invariant
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


# George & Hwang (2004), "The 52-Week High and Momentum Investing", JF 59(5), 2145-2176: https://doi.org/10.1111/j.1540-6261.2004.00695.x
# Adaptation: close over trailing window high minus one, with a configurable bar window.
@_scale_invariant
def distance_to_high(close, window: int = 252):
    """Close / highest close in the window - 1; needs window bars."""
    return _ratio(close, _apply(close, window, lambda x: x.max(axis=1))) - 1


# Baz, Granger, Harvey, Le Roux & Rattray (2015), "Dissecting Investment Strategies in the Cross Section and Time Series": https://ssrn.com/abstract=2695101
# Adaptation: finite-window mean simple return divided by its standard error (return t-statistic).
@_scale_invariant
def momentum_strength(close, window: int = 60):
    """Mean simple return / standard error (a return t-statistic); needs window + 1 bars."""
    changes = _returns(close)
    mean = _apply(changes, window, lambda x: x.mean(axis=1), 2)
    error = _apply(changes, window, lambda x: x.std(axis=1, ddof=1), 2) / np.sqrt(window)
    return _ratio(mean, error)


# Baz, Granger, Harvey, Le Roux & Rattray (2015), "Dissecting Investment Strategies in the Cross Section and Time Series", Eqs. (4)-(6): https://ssrn.com/abstract=2695101
# Carver (2015), "Systematic Trading", Ch. 7 (EWMAC): https://www.systematicmoney.org/systematic-trading
# Adaptation: normalized finite-window exponential weights over the trailing `slow` closes, scaled by `close * volatility(close, slow)`.
@_scale_invariant
def ewmac(close, fast: int = 16, slow: int = 64):
    """Volatility-normalized fast-minus-slow exponential average crossover; needs slow + 1 bars."""
    if not 0 < fast < slow:
        raise ValueError("ewmac needs 0 < fast < slow")
    ages = np.arange(slow - 1, -1, -1, dtype=float)
    wf = (1.0 - 2.0 / (fast + 1.0)) ** ages
    wf /= wf.sum()
    ws = (1.0 - 2.0 / (slow + 1.0)) ** ages
    ws /= ws.sum()
    diff = wf - ws
    gap = _apply(close, slow, lambda x: (x - x[:, :1]) @ diff)
    scale = np.asarray(close, dtype=float) * volatility(close, slow)
    return _ratio(gap, scale, neutral=0.0)


# Lou, Polk & Skouras (2019), "A tug of war: Overnight versus intraday expected returns", JFE 134(1), 192-213, Eqs. (1)-(2): https://doi.org/10.1016/j.jfineco.2019.03.011
# Adaptation: compounded open-to-prior-close returns over configurable futures bars (including session gaps).
@_scale_invariant
def overnight_return(open, close, window: int = 20):
    """Compounded open / previous-close return over `window` bars; needs window + 1 bars."""
    ratios = np.full(len(close), np.nan)
    ratios[1:] = _ratio(open[1:], close[:-1])
    ratios[1:][np.asarray(open[1:]) <= 0] = np.nan
    return _apply(ratios, window, lambda x: np.prod(x, axis=1) - 1.0)


# Lou, Polk & Skouras (2019), "A tug of war: Overnight versus intraday expected returns", JFE 134(1), 192-213, Eqs. (1)-(2): https://doi.org/10.1016/j.jfineco.2019.03.011
# Adaptation: compounded open-to-close returns over configurable futures bars.
@_scale_invariant
def intraday_return(open, close, window: int = 20):
    """Compounded close / open return over `window` bars; needs window bars."""
    ratios = _ratio(close, open)
    ratios[np.asarray(close) <= 0] = np.nan
    return _apply(ratios, window, lambda x: np.prod(x, axis=1) - 1.0)


# Da, Gurun & Warachka (2014), "Frog in the Pan: Continuous Information and Momentum", Eq. (1): https://doi.org/10.1093/rfs/hhu003
# Adaptation: a configurable bar window, with zero-return bars retained in the denominator.
@_scale_invariant
def information_discreteness(close, window: int = 126):
    """Return sign times (negative fraction - positive fraction); needs window + 1 bars."""
    balance = _apply(_returns(close), window, lambda x: ((x < 0).sum(axis=1) - (x > 0).sum(axis=1)) / window)
    cumulative = np.full(len(close), np.nan)
    cumulative[window:] = _ratio(close[window:], close[:-window]) - 1
    return np.sign(cumulative) * balance


@_scale_invariant
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
@_scale_invariant
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


# Hurst (1951), "Long-Term Storage Capacity of Reservoirs", Trans. ASCE 116, 770-799: https://doi.org/10.1061/TACEAT.0006518
# Di Matteo (2007), "Multi-scaling in finance", Quantitative Finance 7(1), 21-36, Eq. (2): https://doi.org/10.1080/14697680600969727
# Adaptation: finite rolling q=2 log-price structure-function slope via asterion_strategy.ts_hurst.
@_scale_invariant
def hurst_exponent(close, window: int = 100, max_lag: int = 20):
    """Generalized Hurst exponent of log closes (< 0.5 mean-reverting, > 0.5 persistent); needs window bars."""
    return _ts_hurst(close, window, max_lag)


# Uhlenbeck & Ornstein (1930), "On the Theory of the Brownian Motion", Phys. Rev. 36(5), 823-841: https://doi.org/10.1103/PhysRev.36.823
# Adaptation: rolling discrete AR(1) OLS half-life on log closes; non-reverting windows return NaN.
@_scale_invariant
def mean_reversion_half_life(close, window: int = 60):
    """Ornstein-Uhlenbeck mean-reversion half-life of log closes in bars; needs window + 1 bars."""
    return _ou_half_life(_log(close), window)


# Goulding, Harvey & Mazzoleni (2023), "Momentum Turning Points", Eq. (11): https://doi.org/10.1016/j.jfineco.2023.05.007
# Adaptation: static blending of futures-bar mean-return signs with configurable windows.
@_scale_invariant
def blended_momentum(close, fast: int = 21, slow: int = 252, fast_share: float = 0.5):
    """Goulding-Harvey-Mazzoleni (2023) slow/fast sign blend; needs slow + 1 bars."""
    if not 0 < fast < slow or not 0 <= fast_share <= 1:
        raise ValueError("blend needs 0 < fast < slow and fast_share in [0, 1]")
    changes = _returns(close)
    def signal(window):
        mean = _apply(changes, window, lambda x: x.mean(axis=1))
        return np.where(np.isfinite(mean), np.where(mean >= 0, 1.0, -1.0), np.nan)
    return (1 - fast_share) * signal(slow) + fast_share * signal(fast)
