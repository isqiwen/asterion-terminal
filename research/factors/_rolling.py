"""Finite trailing windows shared by the built-in factors."""
import numpy as np


def apply(values, window, reduce, minimum=1):
    if window < minimum:
        raise ValueError(f"window must be at least {minimum}")
    result = np.full(len(values), np.nan)
    if len(values) >= window:
        chunks = np.lib.stride_tricks.sliding_window_view(values, window)
        valid = np.isfinite(chunks).all(axis=1)
        result[window - 1:][valid] = reduce(chunks[valid])
    return result


def ratio(numerator, denominator, neutral=np.nan):
    numerator, denominator = np.broadcast_arrays(numerator, denominator)
    result = np.full(numerator.shape, np.nan)
    finite = np.isfinite(numerator) & np.isfinite(denominator)
    np.divide(numerator, denominator, out=result, where=finite & (denominator > 0))
    result[finite & (denominator == 0) & (numerator == 0)] = neutral
    return result


def returns(close):
    result = np.full(len(close), np.nan)
    result[1:] = ratio(close[1:], close[:-1]) - 1
    result[1:][np.asarray(close[1:]) <= 0] = np.nan
    return result


def log(values):
    result = np.full(len(values), np.nan)
    np.log(values, out=result, where=np.isfinite(values) & (np.asarray(values) > 0))
    return result
