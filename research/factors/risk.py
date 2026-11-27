"""Return distribution and OHLC volatility estimators, without annualization."""
import numpy as np

from asterion_strategy import scale_invariant as _scale_invariant

from factors._rolling import apply as _apply, log as _log, ratio as _ratio, returns as _returns


# Bali, Cakici & Whitelaw (2011), "Maxing out: Stocks as lotteries and the cross-section of expected returns": https://doi.org/10.1016/j.jfineco.2010.08.014
# Adaptation: maximum simple return over configurable futures bars instead of the prior equity month.
@_scale_invariant
def maximum_return(close, window: int = 20):
    """Largest one-bar simple return in the window (MAX); needs window + 1 bars."""
    return _apply(_returns(close), window, lambda x: x.max(axis=1))


@_scale_invariant
def downside_volatility(close, window: int = 20):
    """Root mean squared negative simple returns; needs window + 1 bars."""
    return np.sqrt(_apply(np.minimum(_returns(close), 0) ** 2, window, lambda x: x.mean(axis=1)))


@_scale_invariant
def upside_volatility(close, window: int = 20):
    """Root mean squared positive simple returns; needs window + 1 bars."""
    return np.sqrt(_apply(np.maximum(_returns(close), 0) ** 2, window, lambda x: x.mean(axis=1)))


# Signal source: Fernandez-Perez et al. (2018), "The Skewness of Commodity Futures Returns": https://doi.org/10.1016/j.jbankfin.2017.06.015
@_scale_invariant
def realized_skewness(close, window: int = 60):
    """Standardized third central moment of simple returns; needs window + 1 bars."""
    def skew(x):
        centered = x - x.mean(axis=1, keepdims=True)
        return _ratio((centered ** 3).mean(axis=1), (centered ** 2).mean(axis=1) ** 1.5)
    return _apply(_returns(close), window, skew, 3)


@_scale_invariant
def excess_kurtosis(close, window: int = 60):
    """Standardized fourth central moment minus three; needs window + 1 bars."""
    def kurtosis(x):
        centered = x - x.mean(axis=1, keepdims=True)
        return _ratio((centered ** 4).mean(axis=1), (centered ** 2).mean(axis=1) ** 2) - 3
    return _apply(_returns(close), window, kurtosis, 4)


@_scale_invariant
def expected_shortfall(close, window: int = 60, tail: float = 0.05):
    """Negative mean of the worst ceil(window * tail) returns; needs window + 1 bars."""
    if not 0 < tail <= 1:
        raise ValueError("tail must be in (0, 1]")
    count = int(np.ceil(window * tail))
    return _apply(_returns(close), window, lambda x: -np.sort(x, axis=1)[:, :count].mean(axis=1))


@_scale_invariant
def semivariance_balance(close, window: int = 20):
    """(Positive minus negative realized variance) / total variance; needs window + 1 bars."""
    changes = _returns(close)
    signed = _apply(np.sign(changes) * changes ** 2, window, lambda x: x.sum(axis=1))
    total = _apply(changes ** 2, window, lambda x: x.sum(axis=1))
    return _ratio(signed, total, neutral=0.0)


# Parkinson (1980), "The Extreme Value Method for Estimating the Variance of the Rate of Return": https://doi.org/10.1086/296071
@_scale_invariant
def parkinson_volatility(high, low, window: int = 20):
    """Parkinson high-low log-range volatility; needs window bars."""
    variance = (_log(high) - _log(low)) ** 2 / (4 * np.log(2))
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))


# Garman & Klass (1980), "On the Estimation of Security Price Volatilities from Historical Data": https://doi.org/10.1086/296072
# Uses the paper's simplified range/open-close estimator.
@_scale_invariant
def garman_klass_volatility(open, high, low, close, window: int = 20):
    """Garman-Klass OHLC volatility, excluding overnight gaps; needs window bars."""
    variance = 0.5 * (_log(high) - _log(low)) ** 2 \
        - (2 * np.log(2) - 1) * (_log(close) - _log(open)) ** 2
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))


# Rogers & Satchell (1991), "Estimating Variance From High, Low and Closing Prices": https://doi.org/10.1214/aoap/1177005835
# Uses the continuous-path base estimator.
@_scale_invariant
def rogers_satchell_volatility(open, high, low, close, window: int = 20):
    """Rogers-Satchell drift-independent OHLC volatility; needs window bars."""
    log_high, log_low, log_open, log_close = _log(high), _log(low), _log(open), _log(close)
    variance = (log_high - log_open) * (log_high - log_close) \
        + (log_low - log_open) * (log_low - log_close)
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))


# Yang & Zhang (2000), "Drift-Independent Volatility Estimation Based on High, Low, Open, and Close Prices", Eqs. (7), (18): https://doi.org/10.1086/209650
# Adaptation: rolling per-bar volatility, with no annualization.
@_scale_invariant
def yang_zhang_volatility(open, high, low, close, window: int = 20):
    """Opening-gap, open-close and Rogers-Satchell volatility; needs window + 1 bars."""
    lo, lc = _log(open), _log(close)
    previous = np.full(len(close), np.nan)
    previous[1:] = lc[:-1]
    overnight = _apply(lo - previous, window, lambda x: x.var(axis=1, ddof=1), 2)
    intraday = _apply(lc - lo, window, lambda x: x.var(axis=1, ddof=1), 2)
    k = 0.34 / (1.34 + (window + 1) / (window - 1))
    rs = rogers_satchell_volatility(open, high, low, close, window) ** 2
    return np.sqrt(overnight + k * intraday + (1 - k) * rs)
