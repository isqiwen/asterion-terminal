"""Return distribution and OHLC volatility estimators, without annualization."""
import numpy as np

from factors._rolling import apply as _apply, log as _log, ratio as _ratio, returns as _returns


def downside_volatility(close, window: int = 20):
    """Root mean squared negative simple returns; needs window + 1 bars."""
    return np.sqrt(_apply(np.minimum(_returns(close), 0) ** 2, window, lambda x: x.mean(axis=1)))


def upside_volatility(close, window: int = 20):
    """Root mean squared positive simple returns; needs window + 1 bars."""
    return np.sqrt(_apply(np.maximum(_returns(close), 0) ** 2, window, lambda x: x.mean(axis=1)))


def realized_skewness(close, window: int = 60):
    """Standardized third central moment of simple returns; needs window + 1 bars."""
    def skew(x):
        centered = x - x.mean(axis=1, keepdims=True)
        return _ratio((centered ** 3).mean(axis=1), (centered ** 2).mean(axis=1) ** 1.5)
    return _apply(_returns(close), window, skew, 3)


def excess_kurtosis(close, window: int = 60):
    """Standardized fourth central moment minus three; needs window + 1 bars."""
    def kurtosis(x):
        centered = x - x.mean(axis=1, keepdims=True)
        return _ratio((centered ** 4).mean(axis=1), (centered ** 2).mean(axis=1) ** 2) - 3
    return _apply(_returns(close), window, kurtosis, 4)


def expected_shortfall(close, window: int = 60, tail: float = 0.05):
    """Negative mean of the worst ceil(window * tail) returns; needs window + 1 bars."""
    if not 0 < tail <= 1:
        raise ValueError("tail must be in (0, 1]")
    count = int(np.ceil(window * tail))
    return _apply(_returns(close), window, lambda x: -np.sort(x, axis=1)[:, :count].mean(axis=1))


def semivariance_balance(close, window: int = 20):
    """(Positive minus negative realized variance) / total variance; needs window + 1 bars."""
    changes = _returns(close)
    signed = _apply(np.sign(changes) * changes ** 2, window, lambda x: x.sum(axis=1))
    total = _apply(changes ** 2, window, lambda x: x.sum(axis=1))
    return _ratio(signed, total, neutral=0.0)


def parkinson_volatility(high, low, window: int = 20):
    """Parkinson high-low log-range volatility; needs window bars."""
    variance = (_log(high) - _log(low)) ** 2 / (4 * np.log(2))
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))


def garman_klass_volatility(open, high, low, close, window: int = 20):
    """Garman-Klass OHLC volatility, excluding overnight gaps; needs window bars."""
    variance = 0.5 * (_log(high) - _log(low)) ** 2 \
        - (2 * np.log(2) - 1) * (_log(close) - _log(open)) ** 2
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))


def rogers_satchell_volatility(open, high, low, close, window: int = 20):
    """Rogers-Satchell drift-independent OHLC volatility; needs window bars."""
    log_high, log_low, log_open, log_close = _log(high), _log(low), _log(open), _log(close)
    variance = (log_high - log_open) * (log_high - log_close) \
        + (log_low - log_open) * (log_low - log_close)
    return np.sqrt(_apply(variance, window, lambda x: x.mean(axis=1)))
