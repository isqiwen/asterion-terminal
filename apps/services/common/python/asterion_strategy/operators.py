"""Strictly causal time-series, state-space, regime and microstructure operators."""
import math
import numbers

import numpy as np

try:
    import asterion_engine as _engine
except ImportError:
    _engine = None


def ts_mean(values, window: int) -> np.ndarray:
    """Strictly causal trailing arithmetic mean over `window` bars."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1 :] = np.where(valid, np.mean(safe, axis=1), np.nan)
    return out


def ts_std(values, window: int, ddof: int = 1) -> np.ndarray:
    """Strictly causal trailing standard deviation over `window` bars."""
    if type(window) is not int or type(ddof) is not int or ddof < 0 or window <= ddof:
        raise ValueError("window must be an integer greater than ddof")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1 :] = np.where(valid, np.std(safe, axis=1, ddof=ddof), np.nan)
    return out


def ts_delta(values, periods: int = 1) -> np.ndarray:
    """Strictly causal difference `values[t] - values[t - periods]`."""
    if type(periods) is not int or periods < 1:
        raise ValueError("periods must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) > periods:
        diff = arr[periods:] - arr[:-periods]
        out[periods:] = np.where(np.isfinite(diff), diff, np.nan)
    return out


def ts_pct_change(values, periods: int = 1) -> np.ndarray:
    """Strictly causal relative change `values[t] / values[t - periods] - 1`."""
    if type(periods) is not int or periods < 1:
        raise ValueError("periods must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) > periods:
        cur, prev = arr[periods:], arr[:-periods]
        ok = np.isfinite(cur) & np.isfinite(prev) & (prev != 0.0)
        ratio = np.divide(cur, prev, out=np.full_like(cur, np.nan), where=ok)
        out[periods:] = np.where(ok, ratio - 1.0, np.nan)
    return out


def ts_rank(values, window: int) -> np.ndarray:
    """Strictly causal trailing percentile rank of the current bar in `[0, 1]`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    if window == 1:
        out[:] = np.where(valid, 0.5, np.nan)
        return out
    cur = win[:, -1:]
    less = np.sum(win < cur, axis=1)
    equal = np.sum(win == cur, axis=1)
    out[window - 1 :] = np.where(valid, (less + 0.5 * (equal - 1.0)) / (window - 1.0), np.nan)
    return out


def ts_zscore(values, window: int) -> np.ndarray:
    """Strictly causal trailing z-score `(x - mean) / std` over `window` bars."""
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    mu = np.mean(safe, axis=1)
    sigma = np.std(safe, axis=1, ddof=1)
    z = np.divide(safe[:, -1] - mu, sigma, out=np.zeros(len(win), dtype=float), where=sigma > 0)
    out[window - 1 :] = np.where(valid, z, np.nan)
    return out


def ts_corr(x, y, window: int) -> np.ndarray:
    """Strictly causal trailing Pearson correlation over `window` bars."""
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    a = np.asarray(x, dtype=float)
    b = np.asarray(y, dtype=float)
    if a.ndim != 1 or a.shape != b.shape:
        raise ValueError("ts_corr inputs must be 1D arrays of equal length")
    out = np.full(len(a), np.nan)
    if len(a) < window:
        return out
    wa = np.lib.stride_tricks.sliding_window_view(a, window)
    wb = np.lib.stride_tricks.sliding_window_view(b, window)
    valid = np.all(np.isfinite(wa), axis=1) & np.all(np.isfinite(wb), axis=1)
    sa = np.where(valid[:, None], wa, 0.0)
    sb = np.where(valid[:, None], wb, 0.0)
    da = sa - np.mean(sa, axis=1, keepdims=True)
    db = sb - np.mean(sb, axis=1, keepdims=True)
    denom = np.sqrt(np.sum(da * da, axis=1) * np.sum(db * db, axis=1))
    corr = np.divide(np.sum(da * db, axis=1), denom, out=np.full(len(wa), np.nan), where=denom > 0)
    out[window - 1 :] = np.where(valid, np.clip(corr, -1.0, 1.0), np.nan)
    return out


def decay_linear(values, window: int) -> np.ndarray:
    """Strictly causal linearly weighted moving average with weights `1..window`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    weights = np.arange(1, window + 1, dtype=float)
    weights /= weights.sum()
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    out[window - 1 :] = np.where(valid, safe @ weights, np.nan)
    return out


def _annualize_scale(annualize: float) -> float:
    if (
        not isinstance(annualize, numbers.Real)
        or isinstance(annualize, bool)
        or not math.isfinite(annualize)
        or annualize < 0
    ):
        raise ValueError("annualize must be a nonnegative finite number")
    return math.sqrt(float(annualize)) if annualize > 0 else 1.0


def _positive_log(values) -> np.ndarray:
    arr = np.asarray(values, dtype=float)
    ok = np.isfinite(arr) & (arr > 0.0)
    return np.where(ok, np.log(np.where(ok, arr, 1.0)), np.nan)


def ts_ewma_vol(close, span: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal EWMA volatility of log returns (`alpha = 2 / (span + 1)`)."""
    if type(span) is not int or span < 2:
        raise ValueError("span must be an integer of at least 2")
    scale = _annualize_scale(annualize)
    arr = np.asarray(close, dtype=float)
    if _engine is not None and arr.ndim == 1:
        return _engine.ts_ewma_vol(arr, span, scale)
    out = np.full(len(arr), np.nan)
    if len(arr) < 2:
        return out
    alpha = 2.0 / (span + 1.0)
    var = None
    seen = 0
    for i in range(1, len(arr)):
        prev, cur = arr[i - 1], arr[i]
        if not (math.isfinite(prev) and math.isfinite(cur) and prev > 0 and cur > 0):
            continue
        r = math.log(cur / prev)
        sq = r * r
        var = sq if var is None else (1.0 - alpha) * var + alpha * sq
        seen += 1
        if seen >= span:
            out[i] = math.sqrt(var) * scale
    return out


def ts_ema(values, span: int) -> np.ndarray:
    """Strictly causal exponential moving average (`alpha = 2 / (span + 1)`)."""
    if type(span) is not int or span < 1:
        raise ValueError("span must be a positive integer")
    arr = np.asarray(values, dtype=float)
    if _engine is not None and arr.ndim == 1:
        return _engine.ts_ema(arr, span)
    out = np.full(len(arr), np.nan)
    alpha = 2.0 / (span + 1.0)
    ema = None
    seen = 0
    for i, val in enumerate(arr):
        if not math.isfinite(val):
            continue
        ema = float(val) if ema is None else (1.0 - alpha) * ema + alpha * float(val)
        seen += 1
        if seen >= span:
            out[i] = ema
    return out


def _rolling_ols(y, x, window: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    if type(window) is not int or window < 2:
        raise ValueError("window must be an integer of at least 2")
    ay = np.asarray(y, dtype=float)
    ax = np.asarray(x, dtype=float)
    if ay.ndim != 1 or ay.shape != ax.shape:
        raise ValueError("rolling OLS inputs must be 1D arrays of equal length")
    n = len(ay)
    beta_out = np.full(n, np.nan)
    resid_out = np.full(n, np.nan)
    r2_out = np.full(n, np.nan)
    if n < window:
        return beta_out, resid_out, r2_out
    wy = np.lib.stride_tricks.sliding_window_view(ay, window)
    wx = np.lib.stride_tricks.sliding_window_view(ax, window)
    valid = np.all(np.isfinite(wy), axis=1) & np.all(np.isfinite(wx), axis=1)
    sy = np.where(valid[:, None], wy, 0.0)
    sx = np.where(valid[:, None], wx, 0.0)
    my = np.mean(sy, axis=1)
    mx = np.mean(sx, axis=1)
    dy = sy - my[:, None]
    dx = sx - mx[:, None]
    sxx = np.sum(dx * dx, axis=1)
    syy = np.sum(dy * dy, axis=1)
    sxy = np.sum(dx * dy, axis=1)
    ok_x = valid & (sxx > 1e-24)
    beta = np.divide(sxy, sxx, out=np.full(len(wy), np.nan), where=ok_x)
    alpha = my - beta * mx
    resid = sy[:, -1] - (alpha + beta * sx[:, -1])
    ok_r2 = ok_x & (syy > 1e-24)
    r2 = np.divide(sxy * sxy, sxx * syy, out=np.full(len(wy), np.nan), where=ok_r2)
    beta_out[window - 1 :] = np.where(ok_x, beta, np.nan)
    resid_out[window - 1 :] = np.where(ok_x, resid, np.nan)
    r2_out[window - 1 :] = np.where(ok_r2, np.clip(r2, 0.0, 1.0), np.nan)
    return beta_out, resid_out, r2_out


def ts_ols_beta(y, x, window: int) -> np.ndarray:
    """Strictly causal rolling OLS slope of `y = alpha + beta * x` over `window` bars."""
    return _rolling_ols(y, x, window)[0]


def ts_ols_resid(y, x, window: int) -> np.ndarray:
    """Strictly causal trailing OLS residual `y_t - (alpha + beta * x_t)` over `window` bars."""
    return _rolling_ols(y, x, window)[1]


def ts_ols_r2(y, x, window: int) -> np.ndarray:
    """Strictly causal rolling OLS R-squared of `y` on `x` over `window` bars."""
    return _rolling_ols(y, x, window)[2]


def kalman_filter(
    values,
    *,
    process_var: float = 1e-4,
    obs_var: float = 1e-2,
) -> np.ndarray:
    """Strictly causal 1D local-level Kalman filter.

    Reference:
        Kalman, R. E. (1960). A New Approach to Linear Filtering and Prediction
        Problems. Journal of Basic Engineering, 82(1), 35-45.
        https://doi.org/10.1115/1.3662552

    Equations:
        State: x_t = x_{t-1} + w_t,  w_t ~ N(0, Q)
        Obs:   z_t = x_t + v_t,      v_t ~ N(0, R)
        Predict: P_{t|t-1} = P_{t-1|t-1} + Q
        Update:  K_t = P_{t|t-1} / (P_{t|t-1} + R),
                 x_{t|t} = x_{t|t-1} + K_t (z_t - x_{t|t-1}),
                 P_{t|t} = (1 - K_t) P_{t|t-1}.

    Project adaptation:
        Initialized on the first finite observation (x_0 = z_0, P_0 = R);
        missing bars propagate prediction variance without overwriting state and emit NaN.
    """
    if (
        not isinstance(process_var, numbers.Real)
        or isinstance(process_var, bool)
        or not math.isfinite(process_var)
        or process_var < 0.0
    ):
        raise ValueError("process_var must be a nonnegative finite number")
    if (
        not isinstance(obs_var, numbers.Real)
        or isinstance(obs_var, bool)
        or not math.isfinite(obs_var)
        or obs_var <= 0.0
    ):
        raise ValueError("obs_var must be a positive finite number")
    q, r = float(process_var), float(obs_var)
    arr = np.asarray(values, dtype=float)
    if _engine is not None and arr.ndim == 1:
        return _engine.kalman_filter(arr, q, r)
    out = np.full(len(arr), np.nan)
    x_hat = None
    p = r
    for i, z in enumerate(arr):
        if not math.isfinite(z):
            if x_hat is not None:
                p += q
            continue
        if x_hat is None:
            x_hat = float(z)
            p = r
        else:
            p_pred = p + q
            gain = p_pred / (p_pred + r)
            x_hat = x_hat + gain * (float(z) - x_hat)
            p = (1.0 - gain) * p_pred
        out[i] = x_hat
    return out


def kalman_hedge(
    y,
    x,
    *,
    delta: float = 1e-4,
    obs_var: float = 1e-3,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Strictly causal 2-state Kalman dynamic regression `y_t = beta_t * x_t + alpha_t + e_t`.

    References:
        Kalman, R. E. (1960). A New Approach to Linear Filtering and Prediction
        Problems. Journal of Basic Engineering, 82(1), 35-45.
        https://doi.org/10.1115/1.3662552
        Pole, A., West, M., & Harrison, J. (1994). Applied Bayesian Forecasting
        and Time Series Analysis. Chapman & Hall.

    Equations:
        theta_t = [beta_t, alpha_t]^T,  theta_t = theta_{t-1} + w_t,  Q = (delta / (1 - delta)) I_2
        H_t = [x_t, 1],  e_t = y_t - H_t theta_{t|t-1},  S_t = H_t P_{t|t-1} H_t^T + R
        K_t = P_{t|t-1} H_t^T / S_t,  theta_{t|t} = theta_{t|t-1} + K_t e_t,
        Joseph update: P_{t|t} = (I - K_t H_t) P_{t|t-1} (I - K_t H_t)^T + K_t R K_t^T.

    Project adaptation:
        Uses a diffuse prior P_0 = 1e6 * I_2 so arbitrary futures price levels
        converge within two bars; returns `(beta, intercept, spread)` where
        `spread[t]` is the strictly causal one-step-ahead innovation `e_t`
        (NaN on the first finite observation).
    """
    if (
        not isinstance(delta, numbers.Real)
        or isinstance(delta, bool)
        or not math.isfinite(delta)
        or not (0.0 < float(delta) < 1.0)
    ):
        raise ValueError("delta must be strictly in (0, 1)")
    if (
        not isinstance(obs_var, numbers.Real)
        or isinstance(obs_var, bool)
        or not math.isfinite(obs_var)
        or float(obs_var) <= 0.0
    ):
        raise ValueError("obs_var must be a positive finite number")
    ay = np.asarray(y, dtype=float)
    ax = np.asarray(x, dtype=float)
    if ay.ndim != 1 or ay.shape != ax.shape:
        raise ValueError("kalman_hedge inputs must be 1D arrays of equal length")
    q = float(delta) / (1.0 - float(delta))
    r = float(obs_var)
    if _engine is not None:
        return _engine.kalman_hedge(ay, ax, q, r)
    n = len(ay)
    beta_out = np.full(n, np.nan)
    intercept_out = np.full(n, np.nan)
    spread_out = np.full(n, np.nan)
    t0, t1 = 0.0, 0.0
    p00, p01, p11 = 1e6, 0.0, 1e6
    seen = False
    for i in range(n):
        yt, xt = ay[i], ax[i]
        if not (math.isfinite(yt) and math.isfinite(xt)):
            if seen:
                p00 += q
                p11 += q
            continue
        p00_p = p00 + q if seen else p00
        p01_p = p01
        p11_p = p11 + q if seen else p11
        xf, yf = float(xt), float(yt)
        err = yf - (t0 * xf + t1)
        if seen:
            spread_out[i] = err
        s_var = xf * (p00_p * xf + p01_p) + (p01_p * xf + p11_p) + r
        k0 = (p00_p * xf + p01_p) / s_var
        k1 = (p01_p * xf + p11_p) / s_var
        t0 += k0 * err
        t1 += k1 * err
        a00, a01 = 1.0 - k0 * xf, -k0
        a10, a11 = -k1 * xf, 1.0 - k1
        b00 = a00 * p00_p + a01 * p01_p
        b01 = a00 * p01_p + a01 * p11_p
        b10 = a10 * p00_p + a11 * p01_p
        b11 = a10 * p01_p + a11 * p11_p
        p00 = b00 * a00 + b01 * a01 + k0 * r * k0
        p01 = 0.5 * ((b00 * a10 + b01 * a11) + (b10 * a00 + b11 * a01)) + k0 * r * k1
        p11 = b10 * a10 + b11 * a11 + k1 * r * k1
        seen = True
        beta_out[i] = t0
        intercept_out[i] = t1
    return beta_out, intercept_out, spread_out


def ts_autocorr(values, window: int, lag: int = 1) -> np.ndarray:
    """Strictly causal trailing Pearson autocorrelation at `lag` over `window` bars."""
    if type(window) is not int or type(lag) is not int or lag < 1 or window < lag + 2:
        raise ValueError("ts_autocorr requires lag >= 1 and window >= lag + 2")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    a, b = safe[:, :-lag], safe[:, lag:]
    da = a - np.mean(a, axis=1, keepdims=True)
    db = b - np.mean(b, axis=1, keepdims=True)
    denom = np.sqrt(np.sum(da * da, axis=1) * np.sum(db * db, axis=1))
    corr = np.divide(np.sum(da * db, axis=1), denom, out=np.full(len(win), np.nan), where=denom > 1e-24)
    out[window - 1 :] = np.where(valid, np.clip(corr, -1.0, 1.0), np.nan)
    return out


def ts_skew(values, window: int) -> np.ndarray:
    """Strictly causal trailing skewness (standardized 3rd central moment) over `window` bars."""
    if type(window) is not int or window < 3:
        raise ValueError("ts_skew requires window >= 3")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    m2 = np.mean(centered * centered, axis=1)
    m3 = np.mean(centered**3, axis=1)
    ok = valid & (m2 > 1e-24)
    skew = np.divide(m3, np.power(np.where(ok, m2, 1.0), 1.5), out=np.full(len(win), np.nan), where=ok)
    out[window - 1 :] = np.where(ok, skew, np.nan)
    return out


def ts_kurt(values, window: int) -> np.ndarray:
    """Strictly causal trailing excess kurtosis (`m4 / m2^2 - 3`) over `window` bars."""
    if type(window) is not int or window < 4:
        raise ValueError("ts_kurt requires window >= 4")
    arr = np.asarray(values, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) < window:
        return out
    win = np.lib.stride_tricks.sliding_window_view(arr, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    m2 = np.mean(centered * centered, axis=1)
    m4 = np.mean(centered**4, axis=1)
    ok = valid & (m2 > 1e-24)
    kurt = np.divide(m4, np.where(ok, m2 * m2, 1.0), out=np.full(len(win), np.nan), where=ok) - 3.0
    out[window - 1 :] = np.where(ok, kurt, np.nan)
    return out


def ts_variance_ratio(close, window: int, q: int = 5) -> np.ndarray:
    """Strictly causal Lo-MacKinlay overlapping log-return variance ratio `VR(q)`.

    Reference:
        Lo, A. W., & MacKinlay, A. C. (1988). Stock Market Prices Do Not Follow
        Random Walks: Evidence from a Simple Specification Test. Review of
        Financial Studies, 1(1), 41-66, Eqs. (12)-(13).
        https://doi.org/10.1093/rfs/1.1.41

    Equations:
        sigma_a^2 = sum (r_t - mu)^2 / (n - 1),
        sigma_c^2(q) = sum (sum_{j=0}^{q-1} (r_{t-j} - mu))^2 / (q * (n - q + 1) * (1 - q / n)),
        VR(q) = sigma_c^2(q) / sigma_a^2.

    Project adaptation:
        Operates on rolling windows of `window` log returns (requires `window + 1`
        positive closes); returns NaN when single-period variance is zero.
    """
    if type(window) is not int or type(q) is not int or not (2 <= q < window):
        raise ValueError("ts_variance_ratio requires 2 <= q < window")
    log_p = _positive_log(close)
    out = np.full(len(log_p), np.nan)
    if len(log_p) <= window:
        return out
    rets = np.diff(log_p)
    win = np.lib.stride_tricks.sliding_window_view(rets, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    centered = safe - np.mean(safe, axis=1, keepdims=True)
    var1 = np.sum(centered * centered, axis=1) / float(window - 1)
    sums = np.lib.stride_tricks.sliding_window_view(centered, q, axis=1).sum(axis=2)
    denom_q = float(q) * float(window - q + 1) * (1.0 - float(q) / float(window))
    var_q = np.sum(sums * sums, axis=1) / denom_q
    ok = valid & (var1 > 1e-24)
    vr = np.divide(var_q, var1, out=np.full(len(win), np.nan), where=ok)
    out[window:] = np.where(ok, vr, np.nan)
    return out


def ts_hurst(close, window: int = 100, max_lag: int = 20) -> np.ndarray:
    """Strictly causal rolling generalized Hurst exponent (q=2 structure function).

    References:
        Hurst, H. E. (1951). Long-Term Storage Capacity of Reservoirs.
        Transactions of the ASCE, 116, 770-799.
        https://doi.org/10.1061/TACEAT.0006518
        Di Matteo, T. (2007). Multi-scaling in finance. Quantitative Finance,
        7(1), 21-36, Eq. (2). https://doi.org/10.1080/14697680600969727

    Equations:
        K_2(tau) = mean((log P_{t+tau} - log P_t)^2) ~ tau^{2 H},
        H = slope of 0.5 * log K_2(tau) regressed on log(tau) for tau = 2..L.

    Project adaptation:
        Finite rolling window on log prices with L = min(max_lag, window // 2);
        values < 0.5 indicate mean reversion, ~0.5 random walk, > 0.5 persistence.
    """
    if type(window) is not int or type(max_lag) is not int or window < 8 or max_lag < 3:
        raise ValueError("ts_hurst requires window >= 8 and max_lag >= 3")
    log_p = _positive_log(close)
    out = np.full(len(log_p), np.nan)
    if len(log_p) < window:
        return out
    upper = min(max_lag, window // 2)
    if upper < 3:
        raise ValueError("ts_hurst requires min(max_lag, window // 2) >= 3")
    win = np.lib.stride_tricks.sliding_window_view(log_p, window)
    valid = np.all(np.isfinite(win), axis=1)
    safe = np.where(valid[:, None], win, 0.0)
    lags = np.arange(2, upper + 1, dtype=float)
    log_lags = np.log(lags)
    x_c = log_lags - np.mean(log_lags)
    sxx = float(np.sum(x_c * x_c))
    log_rms = np.empty((len(win), len(lags)), dtype=float)
    ok = valid.copy()
    for col, tau in enumerate(range(2, upper + 1)):
        diffs = safe[:, tau:] - safe[:, :-tau]
        msq = np.mean(diffs * diffs, axis=1)
        ok &= msq > 1e-24
        log_rms[:, col] = 0.5 * np.log(np.where(msq > 1e-24, msq, 1.0))
    y_c = log_rms - np.mean(log_rms, axis=1, keepdims=True)
    slope = (y_c @ x_c) / sxx
    out[window - 1 :] = np.where(ok, slope, np.nan)
    return out


def ou_half_life(series, window: int) -> np.ndarray:
    """Strictly causal Ornstein-Uhlenbeck mean-reversion half-life (in bars).

    Reference:
        Uhlenbeck, G. E., & Ornstein, L. S. (1930). On the Theory of the Brownian
        Motion. Physical Review, 36(5), 823-841.
        https://doi.org/10.1103/PhysRev.36.823

    Equations:
        dS_t = theta (mu - S_t) dt + sigma dW_t,
        Discrete AR(1): delta S_t = alpha + beta * S_{t-1} + eps_t,  beta = exp(-theta) - 1,
        t_{1/2} = -ln(2) / ln(1 + beta)  for -1 < beta < 0.

    Project adaptation:
        Estimated causally via rolling OLS over `window` differences (requires
        `window + 1` points); returns NaN when beta >= 0 (non-reverting) or
        beta <= -1 (oscillatory overshoot).
    """
    if type(window) is not int or window < 3:
        raise ValueError("ou_half_life requires window >= 3")
    arr = np.asarray(series, dtype=float)
    out = np.full(len(arr), np.nan)
    if len(arr) <= window:
        return out
    beta = _rolling_ols(np.diff(arr), arr[:-1], window)[0]
    ok = np.isfinite(beta) & (beta < -1e-12) & (beta > -1.0)
    hl = np.full_like(beta, np.nan)
    hl[ok] = -math.log(2.0) / np.log1p(beta[ok])
    out[1:] = hl
    return out


def parkinson_vol(high, low, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Parkinson high-low range volatility.

    Reference:
        Parkinson, M. (1980). The Extreme Value Method for Estimating the
        Variance of the Rate of Return. The Journal of Business, 53(1), 61-65.
        https://doi.org/10.1086/296071

    Equation:
        sigma_P = sqrt(mean(ln(H_t / L_t)^2) / (4 ln 2)) * sqrt(annualize).
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    scale = _annualize_scale(annualize)
    lh, ll = _positive_log(high), _positive_log(low)
    if lh.ndim != 1 or lh.shape != ll.shape:
        raise ValueError("parkinson_vol inputs must be 1D arrays of equal length")
    var = ts_mean(((lh - ll) ** 2) / (4.0 * math.log(2.0)), window)
    return np.where(np.isfinite(var), np.sqrt(np.maximum(var, 0.0)) * scale, np.nan)


def garman_klass_vol(open, high, low, close, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Garman-Klass OHLC volatility.

    Reference:
        Garman, M. B., & Klass, M. J. (1980). On the Estimation of Security
        Price Volatilities from Historical Data. The Journal of Business,
        53(1), 67-78. https://doi.org/10.1086/296072

    Equations:
        v_t = 0.5 * ln(H_t / L_t)^2 - (2 ln 2 - 1) * ln(C_t / O_t)^2,
        sigma_GK = sqrt(max(mean(v_t), 0)) * sqrt(annualize).
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    scale = _annualize_scale(annualize)
    lo, lh, ll, lc = _positive_log(open), _positive_log(high), _positive_log(low), _positive_log(close)
    if lo.ndim != 1 or not (lo.shape == lh.shape == ll.shape == lc.shape):
        raise ValueError("garman_klass_vol inputs must be 1D arrays of equal length")
    inst = 0.5 * (lh - ll) ** 2 - (2.0 * math.log(2.0) - 1.0) * (lc - lo) ** 2
    var = ts_mean(inst, window)
    return np.where(np.isfinite(var), np.sqrt(np.maximum(var, 0.0)) * scale, np.nan)


def yang_zhang_vol(open, high, low, close, window: int = 20, annualize: float = 252.0) -> np.ndarray:
    """Strictly causal Yang-Zhang drift- and gap-independent OHLC volatility.

    Reference:
        Yang, D., & Zhang, Q. (2000). Drift-Independent Volatility Estimation
        Based on High, Low, Open, and Close Prices. The Journal of Business,
        73(3), 477-492, Eqs. (7), (18). https://doi.org/10.1086/209650

    Equations:
        sigma_YZ^2 = sigma_o^2 + k * sigma_c^2 + (1 - k) * sigma_RS^2,
        k = 0.34 / (1.34 + (n + 1) / (n - 1)).
    """
    if type(window) is not int or window < 2:
        raise ValueError("yang_zhang_vol requires window >= 2")
    scale = _annualize_scale(annualize)
    lo, lh, ll, lc = _positive_log(open), _positive_log(high), _positive_log(low), _positive_log(close)
    if lo.ndim != 1 or not (lo.shape == lh.shape == ll.shape == lc.shape):
        raise ValueError("yang_zhang_vol inputs must be 1D arrays of equal length")
    prev_c = np.full(len(lc), np.nan)
    prev_c[1:] = lc[:-1]
    overnight = ts_std(lo - prev_c, window, ddof=1) ** 2
    intraday = ts_std(lc - lo, window, ddof=1) ** 2
    rs = ts_mean((lh - lo) * (lh - lc) + (ll - lo) * (ll - lc), window)
    k = 0.34 / (1.34 + float(window + 1) / float(window - 1))
    total = overnight + k * intraday + (1.0 - k) * rs
    return np.where(np.isfinite(total), np.sqrt(np.maximum(total, 0.0)) * scale, np.nan)


def corwin_schultz_spread(high, low, window: int = 1) -> np.ndarray:
    """Strictly causal Corwin-Schultz high-low implied bid-ask spread estimator.

    Reference:
        Corwin, S. A., & Schultz, P. (2012). A Simple Way to Estimate Bid-Ask
        Spreads from Daily High and Low Prices. The Journal of Finance, 67(2),
        719-760, Eqs. (14)-(18). https://doi.org/10.1111/j.1540-6261.2012.01729.x

    Equations:
        beta_t = ln(H_t / L_t)^2 + ln(H_{t-1} / L_{t-1})^2,
        gamma_t = ln(max(H_t, H_{t-1}) / min(L_t, L_{t-1}))^2,
        alpha_t = max(0, (sqrt(2 beta_t) - sqrt(beta_t)) / (3 - 2 sqrt(2))
                         - sqrt(gamma_t / (3 - 2 sqrt(2)))),
        S_t = 2 (exp(alpha_t) - 1) / (1 + exp(alpha_t)).

    Project adaptation:
        Supports both daily and minute OHLC bars; when `window > 1`, returns the
        causal rolling mean of adjacent-bar spread estimates over `window` bars.
    """
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    ah = np.asarray(high, dtype=float)
    al = np.asarray(low, dtype=float)
    if ah.ndim != 1 or ah.shape != al.shape:
        raise ValueError("corwin_schultz_spread inputs must be 1D arrays of equal length")
    n = len(ah)
    raw_spread = np.full(n, np.nan)
    if n >= 2:
        h0, h1 = ah[:-1], ah[1:]
        l0, l1 = al[:-1], al[1:]
        ok = (
            np.isfinite(h0)
            & np.isfinite(h1)
            & np.isfinite(l0)
            & np.isfinite(l1)
            & (h0 > 0.0)
            & (h1 > 0.0)
            & (l0 > 0.0)
            & (l1 > 0.0)
            & (h0 >= l0)
            & (h1 >= l1)
        )
        sh0, sh1 = np.where(ok, h0, 1.0), np.where(ok, h1, 1.0)
        sl0, sl1 = np.where(ok, l0, 1.0), np.where(ok, l1, 1.0)
        beta = np.log(sh0 / sl0) ** 2 + np.log(sh1 / sl1) ** 2
        gamma = np.log(np.maximum(sh0, sh1) / np.minimum(sl0, sl1)) ** 2
        denom = 3.0 - 2.0 * math.sqrt(2.0)
        alpha = np.maximum(
            0.0, (np.sqrt(2.0 * beta) - np.sqrt(beta)) / denom - np.sqrt(gamma / denom)
        )
        exp_a = np.exp(np.minimum(alpha, 50.0))
        s = 2.0 * (exp_a - 1.0) / (1.0 + exp_a)
        raw_spread[1:] = np.where(ok, s, np.nan)
    return raw_spread if window == 1 else ts_mean(raw_spread, window)


def ts_vwap_dev(close, volume, window: int) -> np.ndarray:
    """Strictly causal deviation from trailing volume-weighted average price `close / VWAP - 1`."""
    if type(window) is not int or window < 1:
        raise ValueError("window must be a positive integer")
    ac = np.asarray(close, dtype=float)
    av = np.asarray(volume, dtype=float)
    if ac.ndim != 1 or ac.shape != av.shape:
        raise ValueError("ts_vwap_dev inputs must be 1D arrays of equal length")
    out = np.full(len(ac), np.nan)
    if len(ac) < window:
        return out
    wc = np.lib.stride_tricks.sliding_window_view(ac, window)
    wv = np.lib.stride_tricks.sliding_window_view(av, window)
    valid = np.all(np.isfinite(wc), axis=1) & np.all(np.isfinite(wv), axis=1) & np.all(wv >= 0.0, axis=1)
    sc = np.where(valid[:, None], wc, 0.0)
    sv = np.where(valid[:, None], wv, 0.0)
    vol_sum = np.sum(sv, axis=1)
    pv_sum = np.sum(sc * sv, axis=1)
    ok = valid & (vol_sum > 0.0) & (pv_sum > 0.0)
    vwap = np.divide(pv_sum, vol_sum, out=np.full(len(wc), np.nan), where=ok)
    out[window - 1 :] = np.where(ok, sc[:, -1] / np.where(ok, vwap, 1.0) - 1.0, np.nan)
    return out
