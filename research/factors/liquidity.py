"""Volume and open-interest signals measured in contracts, not traded currency."""
import numpy as np

from factors._rolling import apply as _apply, ratio as _ratio, returns as _returns


def relative_volume(volume, window: int = 20):
    """Current volume / trailing mean - 1; needs window bars; no-volume windows are missing."""
    return _ratio(volume, _apply(volume, window, lambda x: x.mean(axis=1))) - 1


def volume_weighted_return(close, volume, window: int = 20):
    """Mean one-bar return weighted by contemporaneous volume; needs window + 1 bars."""
    weighted = _apply(_returns(close) * volume, window, lambda x: x.sum(axis=1))
    total = _apply(volume, window, lambda x: x.sum(axis=1))
    return _ratio(weighted, total)


def volume_return_correlation(close, volume, window: int = 20):
    """Pearson correlation of simple return with contemporaneous volume; needs window + 1 bars."""
    if window < 2:
        raise ValueError("correlation needs at least two bars")
    result = np.full(len(close), np.nan)
    if len(close) > window:
        r = np.lib.stride_tricks.sliding_window_view(_returns(close)[1:], window)
        v = np.lib.stride_tricks.sliding_window_view(volume[1:], window)
        r = r - r.mean(axis=1, keepdims=True)
        v = v - v.mean(axis=1, keepdims=True)
        result[window:] = _ratio((r * v).sum(axis=1),
                                 np.sqrt((r * r).sum(axis=1) * (v * v).sum(axis=1)))
    return result


def turnover(volume, open_interest, window: int = 20):
    """Mean volume / open interest, with positive open interest; needs window bars."""
    return _apply(_ratio(volume, open_interest), window, lambda x: x.mean(axis=1))


def return_per_volume(close, volume, window: int = 20):
    """Mean absolute simple return per traded contract; needs window + 1 bars (not dollar Amihud)."""
    return _apply(_ratio(np.abs(_returns(close)), volume), window, lambda x: x.mean(axis=1))


def money_flow_index(high, low, close, volume, window: int = 14):
    """MFI (0..100) from typical price times volume; unchanged prices add no flow; needs window + 1 bars."""
    typical = (high + low + close) / 3
    changes = np.diff(typical, prepend=np.nan)
    flow = typical * volume
    positive = np.where(np.isfinite(changes), flow * (changes > 0), np.nan)
    total = np.where(np.isfinite(changes), flow * (changes != 0), np.nan)
    return 100 * _ratio(_apply(positive, window, lambda x: x.sum(axis=1)),
                        _apply(total, window, lambda x: x.sum(axis=1)), neutral=0.5)
