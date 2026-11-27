"""Finite-window trend and oscillator signals; windows count completed bars."""
import numpy as np

from asterion_strategy import scale_invariant as _scale_invariant

from factors._rolling import apply as _apply, log as _log, ratio as _ratio


@_scale_invariant
def moving_average_gap(close, fast: int = 5, slow: int = 20):
    """Fast simple moving average / slow simple moving average - 1; needs slow bars."""
    if not 0 < fast < slow:
        raise ValueError("moving averages need 0 < fast < slow")
    return _ratio(_apply(close, fast, lambda x: x.mean(axis=1)),
                  _apply(close, slow, lambda x: x.mean(axis=1))) - 1


@_scale_invariant
def price_zscore(close, window: int = 20):
    """Close minus trailing mean, divided by population standard deviation; needs window bars."""
    def score(x):
        shifted = x - x[:, :1]
        return _ratio(shifted[:, -1] - shifted.mean(axis=1), shifted.std(axis=1), neutral=0.0)
    return _apply(close, window, score, 2)


# Wilder (1978), "New Concepts in Technical Trading Systems", Section VI (RSI).
# Adaptation: Cutler finite-window simple sums so research and short-window live outputs match identically.
@_scale_invariant
def rsi(close, window: int = 14):
    """Cutler RSI (simple sums, 0..100); flat windows are 50; needs window + 1 bars."""
    changes = np.diff(close, prepend=np.nan)
    gains = _apply(np.maximum(changes, 0), window, lambda x: x.sum(axis=1))
    moves = _apply(np.abs(changes), window, lambda x: x.sum(axis=1))
    return 100 * _ratio(gains, moves, neutral=0.5)


# Lane (1984), "Lane's Stochastics", Technical Analysis of Stocks & Commodities 2(3), 87-90.
# Adaptation: raw %K over `window` completed bars, returning 50 on zero-range windows.
@_scale_invariant
def stochastic(high, low, close, window: int = 14):
    """Stochastic %K (0..100) over window bars; a zero range is 50."""
    bottom = _apply(low, window, lambda x: x.min(axis=1))
    top = _apply(high, window, lambda x: x.max(axis=1))
    return 100 * _ratio(close - bottom, top - bottom, neutral=0.5)


@_scale_invariant
def channel_position(high, low, close, window: int = 20):
    """Close in the preceding channel: above 1 / below 0 is a breakout; needs window + 1 bars."""
    top = _apply(np.r_[np.nan, high[:-1]] if len(high) else high,
                 window, lambda x: x.max(axis=1))
    bottom = _apply(np.r_[np.nan, low[:-1]] if len(low) else low,
                    window, lambda x: x.min(axis=1))
    return _ratio(close - bottom, top - bottom)


# Kaufman (1995), "Smarter Trading: Improving Performance in Changing Markets", Ch. 4 (AMA Efficiency Ratio).
# Adaptation: finite-window net price displacement divided by summed bar-to-bar absolute moves, returning 0 on flat windows.
@_scale_invariant
def efficiency_ratio(close, window: int = 20):
    """Kaufman efficiency: absolute net move / total absolute moves; needs window + 1 bars."""
    if window < 1:
        raise ValueError("window must be at least one")
    distance = np.full(len(close), np.nan)
    distance[window:] = np.abs(close[window:] - close[:-window])
    travelled = _apply(np.abs(np.diff(close, prepend=np.nan)), window, lambda x: x.sum(axis=1))
    return _ratio(distance, travelled, neutral=0.0)


@_scale_invariant
def log_price_slope(close, window: int = 20):
    """OLS slope of log closes on bar number, in log return per bar; needs window bars."""
    if window < 2:
        raise ValueError("slope needs at least two bars")
    time = np.arange(window, dtype=float) - (window - 1) / 2
    return _apply(_log(close), window, lambda x: ((x - x[:, :1]) @ time) / (time @ time))


# Wilder (1978), "New Concepts in Technical Trading Systems", Section III (True Range / ATR).
# Adaptation: finite-window arithmetic mean of true range instead of Wilder recursive smoothing.
def average_true_range(high, low, close, window: int = 14):
    """Simple mean true range in price units, including gaps; needs window + 1 bars."""
    ranges = np.full(len(close), np.nan)
    ranges[1:] = np.maximum(high[1:] - low[1:],
                            np.maximum(np.abs(high[1:] - close[:-1]),
                                       np.abs(low[1:] - close[:-1])))
    return _apply(ranges, window, lambda x: x.mean(axis=1))


# Lambert (1980), "Commodity Channel Index: Tool for Trading Cyclic Trends", Commodities Magazine 9(10), 40-41.
# Adaptation: typical price deviation scaled by 0.015 * mean absolute deviation, returning 0 on flat windows.
@_scale_invariant
def commodity_channel_index(high, low, close, window: int = 20):
    """CCI of typical price with mean absolute deviation and 0.015 scale; needs window bars."""
    def score(x):
        shifted = x - x[:, :1]
        centered = shifted - shifted.mean(axis=1, keepdims=True)
        return _ratio(centered[:, -1], 0.015 * np.abs(centered).mean(axis=1), neutral=0.0)
    return _apply((high + low + close) / 3, window, score, 2)
