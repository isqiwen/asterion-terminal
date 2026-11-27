"""Return distribution and OHLC volatility estimators, without annualization."""
import numpy as np

from asterion_strategy import (
    safe_log as _log,
    safe_ratio as _ratio,
    scale_invariant as _scale_invariant,
    simple_returns as _returns,
    ts_expected_shortfall as _ts_expected_shortfall,
    ts_kurt as _ts_kurt,
    ts_max as _ts_max,
    ts_mean as _ts_mean,
    ts_skew as _ts_skew,
    ts_std as _ts_std,
)

from factors.price import volatility as _volatility


# Bali, Cakici & Whitelaw (2011), "Maxing out: Stocks as lotteries and the cross-section of expected returns": https://doi.org/10.1016/j.jfineco.2010.08.014
# Adaptation: maximum simple return over configurable futures bars instead of the prior equity month.
@_scale_invariant
def maximum_return(close, window: int = 20):
    """Largest one-bar simple return in the window (MAX); needs window + 1 bars."""
    return _ts_max(_returns(close), window)


# Markowitz (1959), "Portfolio Selection: Efficient Diversification of Investments";
# Barndorff-Nielsen, Kinnebrock & Shephard (2010), "Measuring Downside Risk — Realised Semivariance": https://doi.org/10.1093/acprof:oso/9780199549498.003.0007
# Adaptation: root mean squared negative simple returns over all `window` bars.
@_scale_invariant
def downside_volatility(close, window: int = 20):
    """Root mean squared negative simple returns; needs window + 1 bars."""
    return np.sqrt(_ts_mean(np.minimum(_returns(close), 0) ** 2, window))


# Barndorff-Nielsen, Kinnebrock & Shephard (2010), "Measuring Downside Risk — Realised Semivariance": https://doi.org/10.1093/acprof:oso/9780199549498.003.0007
# Adaptation: root mean squared positive simple returns over all `window` bars.
@_scale_invariant
def upside_volatility(close, window: int = 20):
    """Root mean squared positive simple returns; needs window + 1 bars."""
    return np.sqrt(_ts_mean(np.maximum(_returns(close), 0) ** 2, window))


# Fernandez-Perez, Frijns, Fuertes & Miffre (2018), "The Skewness of Commodity Futures Returns", JBF 86, 143-158, Eq. (1): https://doi.org/10.1016/j.jbankfin.2017.06.015
# Adaptation: population central moments over a configurable trailing futures-bar window.
@_scale_invariant
def realized_skewness(close, window: int = 60):
    """Standardized third central moment of simple returns; needs window + 1 bars."""
    return _ts_skew(_returns(close), window)


@_scale_invariant
def excess_kurtosis(close, window: int = 60):
    """Standardized fourth central moment minus three; needs window + 1 bars."""
    return _ts_kurt(_returns(close), window)


# Acerbi & Tasche (2002), "On the coherence of expected shortfall", JBF 26(7), 1487-1503, Eq. (3.2): https://doi.org/10.1016/S0378-4266(02)00283-2
# Adaptation: empirical mean of the worst ceil(window * tail) simple returns, negated so deeper left tails rank higher.
@_scale_invariant
def expected_shortfall(close, window: int = 60, tail: float = 0.05):
    """Negative mean of the worst ceil(window * tail) returns; needs window + 1 bars."""
    return _ts_expected_shortfall(_returns(close), window, tail)


# Patton & Sheppard (2015), "Good Volatility, Bad Volatility: Signed Jumps and The Persistence of Volatility", REStat 97(3), 683-697, Eq. (4): https://doi.org/10.1162/REST_a_00503
# Adaptation: signed realized semivariance difference normalized by total realized variance `(RS+ - RS-) / RV`.
@_scale_invariant
def semivariance_balance(close, window: int = 20):
    """(Positive minus negative realized variance) / total variance; needs window + 1 bars."""
    changes = _returns(close)
    signed = _ts_mean(np.sign(changes) * changes ** 2, window)
    total = _ts_mean(changes ** 2, window)
    return _ratio(signed, total, neutral=0.0)


@_scale_invariant
def volatility_ratio(close, fast: int = 10, slow: int = 60):
    """Short-window return volatility divided by long-window volatility; needs slow + 1 bars."""
    if not 2 <= fast < slow:
        raise ValueError("volatility ratio needs 2 <= fast < slow")
    return _ratio(_volatility(close, fast), _volatility(close, slow))


# Parkinson (1980), "The Extreme Value Method for Estimating the Variance of the Rate of Return", Eq. (2.6): https://doi.org/10.1086/296071
# Adaptation: rolling per-bar volatility over `window` bars without annualization.
@_scale_invariant
def parkinson_volatility(high, low, window: int = 20):
    """Parkinson high-low log-range volatility; needs window bars."""
    variance = (_log(high) - _log(low)) ** 2 / (4 * np.log(2))
    return np.sqrt(_ts_mean(variance, window))


# Garman & Klass (1980), "On the Estimation of Security Price Volatilities from Historical Data", Eq. (20): https://doi.org/10.1086/296072
# Adaptation: simplified range/open-close estimator averaged over `window` bars without annualization.
@_scale_invariant
def garman_klass_volatility(open, high, low, close, window: int = 20):
    """Garman-Klass OHLC volatility, excluding overnight gaps; needs window bars."""
    variance = 0.5 * (_log(high) - _log(low)) ** 2 \
        - (2 * np.log(2) - 1) * (_log(close) - _log(open)) ** 2
    return np.sqrt(_ts_mean(variance, window))


# Rogers & Satchell (1991), "Estimating Variance From High, Low and Closing Prices", Eq. (3.1): https://doi.org/10.1214/aoap/1177005835
# Adaptation: drift-independent continuous-path estimator averaged over `window` bars without annualization.
@_scale_invariant
def rogers_satchell_volatility(open, high, low, close, window: int = 20):
    """Rogers-Satchell drift-independent OHLC volatility; needs window bars."""
    log_high, log_low, log_open, log_close = _log(high), _log(low), _log(open), _log(close)
    variance = (log_high - log_open) * (log_high - log_close) \
        + (log_low - log_open) * (log_low - log_close)
    return np.sqrt(_ts_mean(variance, window))


# Yang & Zhang (2000), "Drift-Independent Volatility Estimation Based on High, Low, Open, and Close Prices", Eqs. (7), (18): https://doi.org/10.1086/209650
# Adaptation: rolling per-bar volatility, with no annualization.
@_scale_invariant
def yang_zhang_volatility(open, high, low, close, window: int = 20):
    """Opening-gap, open-close and Rogers-Satchell volatility; needs window + 1 bars."""
    if window < 2:
        raise ValueError("window must be at least 2")
    lo, lc = _log(open), _log(close)
    previous = np.full(len(close), np.nan)
    previous[1:] = lc[:-1]
    overnight = _ts_std(lo - previous, window, ddof=1) ** 2
    intraday = _ts_std(lc - lo, window, ddof=1) ** 2
    k = 0.34 / (1.34 + (window + 1) / (window - 1))
    rs = rogers_satchell_volatility(open, high, low, close, window) ** 2
    return np.sqrt(overnight + k * intraday + (1 - k) * rs)
