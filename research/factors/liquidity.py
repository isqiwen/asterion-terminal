"""Volume and open-interest signals measured in contracts, not traded currency."""
import numpy as np

from asterion_strategy import (
    corwin_schultz_spread as _corwin_schultz_spread,
    scale_invariant as _scale_invariant,
    ts_vwap_dev as _ts_vwap_dev,
)

from factors._rolling import apply as _apply, ratio as _ratio, returns as _returns


def relative_volume(volume, window: int = 20):
    """Current volume / trailing mean - 1; needs window bars; no-volume windows are missing."""
    return _ratio(volume, _apply(volume, window, lambda x: x.mean(axis=1))) - 1


@_scale_invariant
def volume_weighted_return(close, volume, window: int = 20):
    """Mean one-bar return weighted by contemporaneous volume; needs window + 1 bars."""
    weighted = _apply(_returns(close) * volume, window, lambda x: x.sum(axis=1))
    total = _apply(volume, window, lambda x: x.sum(axis=1))
    return _ratio(weighted, total)


@_scale_invariant
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


# Amihud (2002), "Illiquidity and stock returns: cross-section and time-series effects", JFM 5(1), 31-56, Eq. (1): https://doi.org/10.1016/S1386-4181(01)00024-6
# Adaptation: contract volume rather than traded currency volume; scale differs across products by contract multiplier.
@_scale_invariant
def return_per_volume(close, volume, window: int = 20):
    """Mean absolute simple return per traded contract; needs window + 1 bars (not dollar Amihud)."""
    return _apply(_ratio(np.abs(_returns(close)), volume), window, lambda x: x.mean(axis=1))


# Quong & Soudack (1989), "Volume-Weighted RSI: Money Flow", Technical Analysis of Stocks & Commodities 7(3), 76-77.
# Adaptation: finite-window sums of positive vs nonzero typical-price money flow; flat windows return 50.
@_scale_invariant
def money_flow_index(high, low, close, volume, window: int = 14):
    """MFI (0..100) from typical price times volume; unchanged prices add no flow; needs window + 1 bars."""
    typical = (high + low + close) / 3
    changes = np.diff(typical, prepend=np.nan)
    flow = typical * volume
    positive = np.where(np.isfinite(changes), flow * (changes > 0), np.nan)
    total = np.where(np.isfinite(changes), flow * (changes != 0), np.nan)
    return 100 * _ratio(_apply(positive, window, lambda x: x.sum(axis=1)),
                        _apply(total, window, lambda x: x.sum(axis=1)), neutral=0.5)


# Marc Chaikin's CMF formula: https://chartschool.stockcharts.com/table-of-contents/technical-indicators-and-overlays/technical-indicators/chaikin-money-flow-cmf
@_scale_invariant
def chaikin_money_flow(high, low, close, volume, window: int = 20):
    """Volume-weighted close location (-1..1), not observed order flow; needs window bars."""
    location = _ratio((close - low) - (high - close), high - low, neutral=0.0)
    weighted = _apply(location * volume, window, lambda x: x.sum(axis=1))
    total = _apply(volume, window, lambda x: x.sum(axis=1))
    return _ratio(weighted, total)


# Corwin & Schultz (2012), "A Simple Way to Estimate Bid-Ask Spreads from Daily High and Low Prices", JF 67(2), 719-760, Eqs. (14)-(18): https://doi.org/10.1111/j.1540-6261.2012.01729.x
# Adaptation: rolling mean of adjacent-bar high-low implied spread estimates via asterion_strategy.corwin_schultz_spread.
@_scale_invariant
def corwin_schultz_spread(high, low, window: int = 20):
    """Corwin-Schultz high-low implied bid-ask spread; needs window + 1 bars."""
    return _corwin_schultz_spread(high, low, window)


@_scale_invariant
def vwap_deviation(close, volume, window: int = 20):
    """Close / trailing volume-weighted average price - 1; needs window bars."""
    return _ts_vwap_dev(close, volume, window)
