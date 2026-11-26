"""Statistical significance and backtest overfitting diagnostics for Lab research.

Pure NumPy/SciPy implementations shared by backtest diagnostics, parameter sweep
overfitting checks, factor evaluation (Newey-West HAC), and walk-forward selection.
"""
import itertools
import math

import numpy as np
from scipy import stats as sp_stats

EULER_MASCHERONI = 0.5772156649015329


def _finite_1d(values) -> np.ndarray:
    arr = np.asarray(values, dtype=float).ravel()
    return arr[np.isfinite(arr)]


def _moments(arr: np.ndarray) -> tuple[float, float, float, float]:
    """Sample mean, sample std (ddof=1), skewness, and excess kurtosis."""
    mean = float(np.mean(arr))
    std = float(np.std(arr, ddof=1))
    if std <= 0.0 or not math.isfinite(std):
        return mean, 0.0, 0.0, 0.0
    z = (arr - mean) / std
    skew = float(np.mean(z ** 3))
    excess_kurt = float(np.mean(z ** 4) - 3.0)
    return mean, std, skew, excess_kurt


def probabilistic_sharpe_ratio(
    returns,
    *,
    benchmark_sr: float = 0.0,
    annual_periods: float = 252.0,
) -> dict | None:
    """Probabilistic Sharpe Ratio (PSR) adjusted for skewness and excess kurtosis.

    Reference:
        Bailey, D. H., & López de Prado, M. (2012). The Sharpe Ratio Efficient
        Frontier. Journal of Risk, 15(2), 3-44.
        https://doi.org/10.21314/JOR.2012.248

    Equations:
        SR_hat = mean(r) / std(r, ddof=1)
        sigma_SR = sqrt((1 - skew * SR_hat + ((kurt_excess + 2) / 4) * SR_hat^2) / (T - 1))
        PSR(SR*) = Phi((SR_hat - SR*) / sigma_SR)

    Project adaptation:
        `benchmark_sr` is passed on the annualized scale when `annual_periods > 0`
        and converted to per-period units before evaluating PSR.
    """
    arr = _finite_1d(returns)
    t_obs = len(arr)
    if t_obs < 4 or annual_periods <= 0.0:
        return None
    mean, std, skew, excess_kurt = _moments(arr)
    if std <= 0.0:
        return None
    sr_period = mean / std
    scale = math.sqrt(float(annual_periods))
    sr_ann = sr_period * scale
    sr_star = float(benchmark_sr) / scale
    # Guard against extreme negative skew/kurt combinations yielding non-positive variance.
    sr_var = (1.0 - skew * sr_period + 0.25 * (excess_kurt + 2.0) * (sr_period ** 2)) / float(t_obs - 1)
    if not math.isfinite(sr_var) or sr_var <= 1e-18:
        sr_var = 1.0 / float(t_obs - 1)
    z_score = (sr_period - sr_star) / math.sqrt(sr_var)
    psr = float(sp_stats.norm.cdf(z_score))
    return {
        "n": t_obs,
        "sharpe": sr_ann,
        "skewness": skew,
        "excess_kurtosis": excess_kurt,
        "psr": psr,
        "z_score": float(z_score),
    }


def deflated_sharpe_ratio(
    returns,
    *,
    trials: int = 1,
    sr_variance: float | None = None,
    annual_periods: float = 252.0,
) -> dict | None:
    """Deflated Sharpe Ratio (DSR) correcting for selection bias and non-normality.

    Reference:
        Bailey, D. H., & López de Prado, M. (2014). The Deflated Sharpe Ratio:
        Correcting for Selection Bias, Backtest Overfitting, and Non-Normality.
        The Journal of Portfolio Management, 40(5), 94-107.
        https://doi.org/10.3905/jpm.2014.40.5.094

    Equations:
        E[max_n SR_n] ~= sqrt(V[{SR_n}]) * ((1 - gamma) * Phi^-1(1 - 1/N)
                                           + gamma * Phi^-1(1 - 1/(N * e)))
        DSR = PSR(SR_0 = E[max_n SR_n])

    Project adaptation:
        When `sr_variance` across sweep candidates is not provided (single-run
        cumulative trial count), uses the theoretical null per-period Sharpe
        variance `1 / (T - 1)` for `N` independent zero-skill trials.
    """
    arr = _finite_1d(returns)
    t_obs = len(arr)
    n_trials = max(1, int(trials))
    base = probabilistic_sharpe_ratio(arr, benchmark_sr=0.0, annual_periods=annual_periods)
    if base is None:
        return None
    scale = math.sqrt(float(annual_periods))
    if n_trials <= 1:
        sr0_ann = 0.0
        dsr_val = base["psr"]
    else:
        z_max = (
            (1.0 - EULER_MASCHERONI) * float(sp_stats.norm.ppf(1.0 - 1.0 / n_trials))
            + EULER_MASCHERONI * float(sp_stats.norm.ppf(1.0 - 1.0 / (n_trials * math.e)))
        )
        if sr_variance is not None and math.isfinite(sr_variance) and sr_variance > 0.0:
            per_period_var = float(sr_variance) / float(annual_periods)
        else:
            per_period_var = 1.0 / float(t_obs - 1)
        sr0_ann = math.sqrt(per_period_var) * z_max * scale
        deflated = probabilistic_sharpe_ratio(
            arr, benchmark_sr=sr0_ann, annual_periods=annual_periods
        )
        dsr_val = deflated["psr"] if deflated is not None else base["psr"]
    return {
        **base,
        "trials": n_trials,
        "expected_max_sharpe": float(sr0_ann),
        "dsr": float(dsr_val),
    }


def newey_west_tstat(series, *, max_lag: int | None = None) -> dict | None:
    """Newey-West HAC-adjusted t-statistic for the mean of an autocorrelated series.

    Reference:
        Newey, W. K., & West, K. D. (1987). A Simple, Positive Semi-Definite,
        Heteroskedasticity and Autocorrelation Consistent Covariance Matrix.
        Econometrica, 55(3), 703-708. https://doi.org/10.2307/1913610

    Equations:
        gamma_l = (1 / T) * sum_{t=l}^{T-1} (x_t - x_bar)(x_{t-l} - x_bar)
        Omega_hat = gamma_0 + 2 * sum_{l=1}^{L} (1 - l / (L + 1)) * gamma_l
        t_HAC = x_bar / sqrt(Omega_hat / (T - 1))

    Project adaptation:
        Uses `T - 1` degrees of freedom so `max_lag=0` matches the ordinary
        sample t-statistic (`ddof=1`); defaults to automatic lag
        `floor(4 * (T / 100)^(2/9))` when `max_lag` is omitted.
    """
    arr = _finite_1d(series)
    t_obs = len(arr)
    if t_obs < 2:
        return None
    mean = float(np.mean(arr))
    u = arr - mean
    lags = (
        min(t_obs - 1, max(0, int(max_lag)))
        if max_lag is not None
        else min(t_obs - 1, max(1, int(math.floor(4.0 * ((t_obs / 100.0) ** (2.0 / 9.0))))))
    )
    gamma0 = float(np.dot(u, u)) / float(t_obs)
    omega = gamma0
    for lag in range(1, lags + 1):
        weight = 1.0 - lag / float(lags + 1)
        cov_l = float(np.dot(u[lag:], u[:-lag])) / float(t_obs)
        omega += 2.0 * weight * cov_l
    if not math.isfinite(omega) or omega <= 0.0:
        return {
            "n": t_obs,
            "lags": lags,
            "mean": mean,
            "se": 0.0,
            "t": None,
            "p_value": None,
        }
    se = math.sqrt(omega / float(t_obs - 1))
    t_val = mean / se
    p_val = float(2.0 * sp_stats.t.sf(abs(t_val), df=t_obs - 1))
    return {
        "n": t_obs,
        "lags": lags,
        "mean": mean,
        "se": se,
        "t": float(t_val),
        "p_value": p_val,
    }


def block_bootstrap_ci(
    returns,
    *,
    annual_periods: float = 252.0,
    block_size: int | None = None,
    n_samples: int = 500,
    alpha: float = 0.05,
    seed: int = 20260401,
) -> dict | None:
    """Circular block bootstrap confidence intervals for return, Sharpe, and drawdown.

    Reference:
        Politis, D. N., & Romano, J. P. (1994). The Stationary Bootstrap.
        Journal of the American Statistical Association, 89(428), 1303-1313.
        https://doi.org/10.1080/01621459.1994.10476870

    Project adaptation:
        Uses fixed-length circular blocks (`b = round(T^(1/3))` by default) with
        a deterministic RNG seed so persisted run diagnostics are reproducible.
    """
    arr = _finite_1d(returns)
    t_obs = len(arr)
    if t_obs < 10 or annual_periods <= 0.0 or n_samples < 20 or not (0.0 < alpha < 0.5):
        return None
    b_len = (
        max(1, min(t_obs, int(block_size)))
        if block_size is not None
        else max(2, min(t_obs // 2, int(round(t_obs ** (1.0 / 3.0)))))
    )
    n_blocks = int(math.ceil(t_obs / b_len))
    rng = np.random.default_rng(seed)
    starts = rng.integers(0, t_obs, size=(n_samples, n_blocks))
    offsets = np.arange(b_len, dtype=np.int64)
    idx = (starts[:, :, None] + offsets[None, None, :]).reshape(n_samples, n_blocks * b_len)[:, :t_obs] % t_obs
    samples = arr[idx]

    scale = math.sqrt(float(annual_periods))
    means = np.mean(samples, axis=1)
    stds = np.std(samples, axis=1, ddof=1)
    valid_sr = stds > 0.0
    sharpes = np.divide(means, stds, out=np.full(n_samples, np.nan), where=valid_sr) * scale

    growth = np.maximum(1.0 + samples, 1e-9)
    log_cum = np.cumsum(np.log(growth), axis=1)
    ann_returns = np.exp(log_cum[:, -1] * (float(annual_periods) / float(t_obs))) - 1.0

    # Prepend initial wealth 1.0 (log wealth 0.0) so initial losses count as drawdowns.
    zeros = np.zeros((n_samples, 1), dtype=float)
    log_path = np.concatenate([zeros, log_cum], axis=1)
    log_peak = np.maximum.accumulate(log_path, axis=1)
    max_dds = np.min(np.exp(log_path - log_peak) - 1.0, axis=1)

    lo_q, hi_q = 100.0 * (alpha / 2.0), 100.0 * (1.0 - alpha / 2.0)

    def ci(vals: np.ndarray) -> list[float] | None:
        finite = vals[np.isfinite(vals)]
        if len(finite) < 10:
            return None
        lo, hi = np.percentile(finite, [lo_q, hi_q])
        return [float(lo), float(hi)]

    return {
        "alpha": float(alpha),
        "block_size": b_len,
        "samples": int(n_samples),
        "sharpe_ci": ci(sharpes),
        "annual_return_ci": ci(ann_returns),
        "max_drawdown_ci": ci(max_dds),
    }


def cscv_pbo(
    return_matrix,
    *,
    n_partitions: int = 10,
    annual_periods: float = 252.0,
) -> dict | None:
    """Probability of Backtest Overfitting (PBO) via Combinatorially Symmetric CV.

    Reference:
        Bailey, D. H., Borwein, J., López de Prado, M., & Zhu, Q. J. (2017).
        The Probability of Backtest Overfitting. Journal of Computational
        Finance, 20(4), 39-69. https://doi.org/10.21314/JCF.2016.322

    Equations:
        Partition T rows into S even chronological blocks. For each half-split
        (IS, OOS) of size S/2:
            n* = argmax_n SR_IS(n)
            w_bar = rank_OOS(n*) / (M + 1) in (0, 1)
            lambda = ln(w_bar / (1 - w_bar))
            PBO = P(lambda <= 0)

    Project adaptation:
        Precomputes per-block sufficient statistics (sum and sum of squares) so
        all C(S, S/2) combinations evaluate in vectorized NumPy; caps S to even
        values <= 12 with at least 4 observations per block.
    """
    mat = np.asarray(return_matrix, dtype=float)
    if mat.ndim != 2:
        raise ValueError("return_matrix must be a 2D array of shape (observations, candidates)")
    mat = mat[np.all(np.isfinite(mat), axis=1)]
    t_obs, n_cand = mat.shape
    if n_cand < 2 or t_obs < 16 or annual_periods <= 0.0:
        return None
    max_s = min(12, max(4, int(n_partitions)))
    if max_s % 2 == 1:
        max_s -= 1
    parts = 0
    for candidate_s in range(max_s, 3, -2):
        if t_obs // candidate_s >= 4:
            parts = candidate_s
            break
    if parts < 4:
        return None
    block_len = t_obs // parts
    used_t = parts * block_len
    trimmed = mat[:used_t].reshape(parts, block_len, n_cand)
    block_sum = np.sum(trimmed, axis=1)        # (S, M)
    block_sq = np.sum(trimmed * trimmed, axis=1)  # (S, M)
    total_sum = np.sum(block_sum, axis=0)      # (M,)
    total_sq = np.sum(block_sq, axis=0)        # (M,)

    half = parts // 2
    half_n = float(half * block_len)
    scale = math.sqrt(float(annual_periods))

    def sharpe_from_sums(s1: np.ndarray, s2: np.ndarray) -> np.ndarray:
        mean = s1 / half_n
        var = np.maximum(0.0, (s2 - half_n * mean * mean) / (half_n - 1.0))
        std = np.sqrt(var)
        return np.divide(mean, std, out=np.zeros_like(mean), where=std > 1e-18) * scale

    logits = []
    rank_corrs = []
    for combo in itertools.combinations(range(parts), half):
        idx = list(combo)
        is_sum = np.sum(block_sum[idx], axis=0)
        is_sq = np.sum(block_sq[idx], axis=0)
        oos_sum = total_sum - is_sum
        oos_sq = total_sq - is_sq
        sr_is = sharpe_from_sums(is_sum, is_sq)
        sr_oos = sharpe_from_sums(oos_sum, oos_sq)

        winner = int(np.argmax(sr_is))
        oos_ranks = sp_stats.rankdata(sr_oos, method="average")
        w_bar = float(oos_ranks[winner]) / float(n_cand + 1)
        w_bar = min(max(w_bar, 1e-9), 1.0 - 1e-9)
        logits.append(math.log(w_bar / (1.0 - w_bar)))

        if n_cand >= 3 and np.std(sr_is) > 0.0 and np.std(sr_oos) > 0.0:
            is_ranks = sp_stats.rankdata(sr_is, method="average")
            corr = float(np.corrcoef(is_ranks, oos_ranks)[0, 1])
            if math.isfinite(corr):
                rank_corrs.append(corr)

    logits_arr = np.asarray(logits, dtype=float)
    return {
        "pbo": float(np.mean(logits_arr <= 0.0)),
        "logits_median": float(np.median(logits_arr)),
        "is_oos_rank_corr": float(np.mean(rank_corrs)) if rank_corrs else None,
        "partitions": parts,
        "combinations": len(logits),
        "candidates": n_cand,
        "observations": used_t,
    }
