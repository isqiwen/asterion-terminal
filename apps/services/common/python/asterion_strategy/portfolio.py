"""Portfolio sizing, risk budgeting, covariance shrinkage and convex optimizers."""
from decimal import Decimal, ROUND_DOWN
import math
import numbers

import numpy as np

from .core import WEIGHT_STEP
from .cross_section import product_sector


def vol_target_weights(
    signals: dict,
    volatilities: dict,
    target_vol: float = 0.10,
    gross_weight: float = 1.0,
    max_weight: float = 0.25,
) -> dict:
    """Scale finite signals by `target_vol / vol`, clip to `max_weight`, and cap gross exposure."""
    if target_vol <= 0 or gross_weight <= 0 or max_weight <= 0:
        raise ValueError("target_vol, gross_weight, and max_weight must be positive")
    raw = {}
    for symbol, signal in signals.items():
        vol = volatilities.get(symbol)
        if (
            isinstance(signal, numbers.Real)
            and not isinstance(signal, bool)
            and math.isfinite(signal)
            and isinstance(vol, numbers.Real)
            and not isinstance(vol, bool)
            and math.isfinite(vol)
            and vol > 0
        ):
            raw[symbol] = float(np.clip(float(signal) * target_vol / float(vol), -max_weight, max_weight))
        else:
            raw[symbol] = 0.0
    gross = sum(abs(v) for v in raw.values())
    scale = min(1.0, gross_weight / gross) if gross > 0 else 1.0
    # Round toward zero so wire precision never exceeds the declared gross budget.
    return {
        symbol: float(Decimal(str(value * scale)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
        for symbol, value in raw.items()
    }


def cap_sector_weights(
    weights: dict,
    max_sector_gross: float = 0.4,
    max_sector_net: float = 0.0,
    sectors: dict | None = None,
) -> dict:
    """Cap per-sector gross and net exposure across China futures sectors without flipping signs."""
    if max_sector_gross < 0 or max_sector_net < 0:
        raise ValueError("sector limits must be nonnegative")
    out = {symbol: float(value) for symbol, value in weights.items()}
    groups: dict[str, list[str]] = {}
    for symbol in out:
        sec = sectors[symbol] if sectors and symbol in sectors else (product_sector(symbol) or "other")
        groups.setdefault(sec, []).append(symbol)
    for members in groups.values():
        if max_sector_gross > 0:
            gross = sum(abs(out[s]) for s in members)
            if gross > max_sector_gross:
                factor = max_sector_gross / gross
                for s in members:
                    out[s] *= factor
        if max_sector_net > 0:
            net = sum(out[s] for s in members)
            if abs(net) > max_sector_net:
                # Trim only the dominant side so opposite hedges are preserved and no sign flips.
                dom = [s for s in members if (out[s] > 0 if net > 0 else out[s] < 0)]
                opp_abs = sum(abs(out[s]) for s in members if s not in dom)
                dom_abs = sum(abs(out[s]) for s in dom)
                target_dom_abs = opp_abs + max_sector_net
                factor = min(1.0, max(0.0, target_dom_abs / dom_abs)) if dom_abs > 0 else 1.0
                for s in dom:
                    out[s] *= factor
    return {
        symbol: float(Decimal(str(value)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN))
        for symbol, value in out.items()
    }


def smooth_weights(
    previous: dict,
    target: dict,
    alpha: float = 0.5,
    deadband: float = 0.0,
) -> dict:
    """Blend `target` with `previous` via `alpha` and suppress same-sign deltas under `deadband`."""
    if not (0.0 < alpha <= 1.0) or deadband < 0.0:
        raise ValueError("alpha must be in (0, 1] and deadband must be nonnegative")
    symbols = set(previous) | set(target)
    out = {}
    for symbol in symbols:
        prev = float(previous.get(symbol, 0.0))
        tgt = float(target.get(symbol, 0.0))
        blended = alpha * tgt + (1.0 - alpha) * prev
        # Never suppress opening from flat, closing to flat, or reversing side.
        same_side = prev != 0.0 and tgt != 0.0 and (prev > 0) == (tgt > 0)
        if deadband > 0.0 and same_side and abs(blended - prev) < deadband:
            out[symbol] = prev
        else:
            out[symbol] = blended
    return out


def drawdown_scale(
    equity_curve,
    soft_dd: float = 0.05,
    hard_dd: float = 0.15,
    min_scale: float = 0.25,
) -> float:
    """Linear risk multiplier in `[min_scale, 1.0]` from trailing peak-to-trough drawdown."""
    if not (0.0 <= soft_dd < hard_dd < 1.0) or not (0.0 <= min_scale <= 1.0):
        raise ValueError("require 0 <= soft_dd < hard_dd < 1 and 0 <= min_scale <= 1")
    arr = np.asarray(equity_curve, dtype=float)
    finite = arr[np.isfinite(arr) & (arr > 0)]
    if len(finite) == 0:
        return 1.0
    peak = float(np.max(finite))
    current = float(finite[-1])
    dd = max(0.0, 1.0 - current / peak)
    if dd <= soft_dd:
        return 1.0
    if dd >= hard_dd:
        return float(min_scale)
    frac = (dd - soft_dd) / (hard_dd - soft_dd)
    return float(1.0 - frac * (1.0 - min_scale))


def combine_weights(allocations: list, symbols) -> dict:
    """Linearly combine `[(weights_by_symbol, share), ...]` across `symbols`."""
    combined = {symbol: 0.0 for symbol in symbols}
    for weights, share in allocations:
        s = float(share)
        if s == 0.0:
            continue
        for symbol, w in weights.items():
            if symbol in combined:
                combined[symbol] += s * float(w)
    return combined


def ledoit_wolf_cov(returns) -> np.ndarray:
    """Linear shrinkage covariance estimator toward the scaled identity matrix.

    Reference:
        Ledoit, O., & Wolf, M. (2004). A well-conditioned estimator for
        large-dimensional covariance matrices. Journal of Multivariate Analysis,
        88(2), 365-411. https://doi.org/10.1016/S0047-259X(03)00096-4

    Equations:
        S = (1 / T) * Y^T Y,  m = tr(S) / N,  d^2 = ||S - m I||_F^2 / N,
        b_bar^2 = (1 / (N T^2)) * sum_t ||y_t y_t^T - S||_F^2,
        b^2 = min(b_bar^2, d^2),  delta = b^2 / d^2,
        Sigma_hat = delta * m * I + (1 - delta) * S.

    Project adaptation:
        Rows with non-finite entries are dropped before estimation; strictly
        causal when fed trailing return history through ctx.time.
    """
    arr = np.asarray(returns, dtype=float)
    if arr.ndim != 2:
        raise ValueError("returns must be a 2D array of shape (observations, assets)")
    arr = arr[np.all(np.isfinite(arr), axis=1)]
    t_obs, n_assets = arr.shape
    if n_assets < 1 or t_obs < 2:
        raise ValueError("ledoit_wolf_cov requires at least 2 finite observations and 1 asset")
    centered = arr - np.mean(arr, axis=0, keepdims=True)
    sample = (centered.T @ centered) / float(t_obs)
    if n_assets == 1:
        return sample
    mu = float(np.trace(sample)) / float(n_assets)
    target = mu * np.eye(n_assets, dtype=float)
    diff = sample - target
    d2 = float(np.sum(diff * diff)) / float(n_assets)
    if d2 <= 1e-24:
        return 0.5 * (sample + sample.T)
    row_norm_sq = np.sum(centered * centered, axis=1)
    sample_frob_sq = float(np.sum(sample * sample))
    b_bar2 = max(0.0, float(np.sum(row_norm_sq * row_norm_sq)) - float(t_obs) * sample_frob_sq) / (
        float(n_assets) * float(t_obs * t_obs)
    )
    delta = min(b_bar2, d2) / d2
    shrunk = delta * target + (1.0 - delta) * sample
    return 0.5 * (shrunk + shrunk.T)


def _ivp_cluster_var(cov: np.ndarray, indices: list[int]) -> float:
    sub = cov[np.ix_(indices, indices)]
    diag = np.maximum(np.diag(sub), 1e-18)
    inv = 1.0 / diag
    w = inv / float(np.sum(inv))
    return float(w @ sub @ w)


def hrp_weights(
    returns,
    *,
    shrinkage: bool = True,
    linkage_method: str = "single",
):
    """Hierarchical Risk Parity (HRP) long-only allocation weights summing to 1.

    Reference:
        López de Prado, M. (2016). Building Diversified Portfolios that
        Outperform Out-of-Sample. The Journal of Portfolio Management, 42(4),
        59-69. https://doi.org/10.3905/jpm.2016.42.4.059

    Equations:
        1. Correlation distance: d_{i,j} = sqrt(0.5 * (1 - rho_{i,j})).
        2. Hierarchical clustering & quasi-diagonalization via dendrogram leaves.
        3. Recursive bisection with inverse-variance cluster risk:
           alpha = 1 - V_left / (V_left + V_right).

    Project adaptation:
        Optionally estimates the covariance matrix with Ledoit-Wolf (2004)
        linear shrinkage (`shrinkage=True` by default) to stabilize small-window
        futures cross-sections; accepts either a `{name: series}` dict or a 2D
        `(observations, assets)` array.
    """
    from scipy.cluster.hierarchy import leaves_list, linkage
    from scipy.spatial.distance import squareform

    names = None
    if isinstance(returns, dict):
        names = list(returns.keys())
        if not names:
            return {}
        arr = np.column_stack([np.asarray(returns[name], dtype=float) for name in names])
    else:
        arr = np.asarray(returns, dtype=float)
        if arr.ndim != 2:
            raise ValueError("returns must be a {name: series} dict or 2D (T, N) array")
    n_assets = arr.shape[1]
    if n_assets == 0:
        return {} if names is not None else np.empty(0, dtype=float)
    if n_assets == 1:
        return {names[0]: 1.0} if names is not None else np.array([1.0], dtype=float)

    finite_rows = arr[np.all(np.isfinite(arr), axis=1)]
    if len(finite_rows) < 2:
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    cov = ledoit_wolf_cov(finite_rows) if shrinkage else np.cov(finite_rows, rowvar=False, ddof=1)
    cov = 0.5 * (np.asarray(cov, dtype=float) + np.asarray(cov, dtype=float).T)
    diag = np.diag(cov)
    if not np.all(np.isfinite(cov)) or np.any(diag <= 1e-18):
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    std = np.sqrt(diag)
    corr = np.clip(cov / np.outer(std, std), -1.0, 1.0)
    np.fill_diagonal(corr, 1.0)
    dist = np.sqrt(np.maximum(0.0, 0.5 * (1.0 - corr)))
    dist = 0.5 * (dist + dist.T)
    np.fill_diagonal(dist, 0.0)
    if n_assets == 2:
        order = [0, 1]
    else:
        condensed = squareform(dist, checks=False)
        order = [int(idx) for idx in leaves_list(linkage(condensed, method=linkage_method))]

    weights = np.ones(n_assets, dtype=float)
    clusters = [order]
    while clusters:
        next_clusters = []
        for cluster in clusters:
            if len(cluster) <= 1:
                continue
            mid = len(cluster) // 2
            left, right = cluster[:mid], cluster[mid:]
            v_left = _ivp_cluster_var(cov, left)
            v_right = _ivp_cluster_var(cov, right)
            denom = v_left + v_right
            alpha = 0.5 if denom <= 0.0 or not math.isfinite(denom) else (1.0 - v_left / denom)
            weights[left] *= alpha
            weights[right] *= 1.0 - alpha
            if len(left) > 1:
                next_clusters.append(left)
            if len(right) > 1:
                next_clusters.append(right)
        clusters = next_clusters

    total = float(np.sum(weights))
    weights = weights / total if total > 0.0 and math.isfinite(total) else np.full(n_assets, 1.0 / n_assets)
    if names is not None:
        return {name: float(weights[i]) for i, name in enumerate(names)}
    return weights


def _project_capped_simplex(values: np.ndarray, upper: float) -> np.ndarray:
    """Euclidean projection onto `{w : 0 <= w_i <= upper, sum(w) = 1}` via bisection."""
    lo = float(np.min(values)) - upper - 1.0
    hi = float(np.max(values)) + 1.0
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if float(np.sum(np.clip(values - mid, 0.0, upper))) > 1.0:
            lo = mid
        else:
            hi = mid
    w = np.clip(values - 0.5 * (lo + hi), 0.0, upper)
    s = float(np.sum(w))
    return w / s if s > 0.0 else np.full(len(values), 1.0 / len(values))


def erc_weights(
    returns,
    *,
    shrinkage: bool = True,
    is_cov: bool = False,
    max_weight: float = 1.0,
):
    """Equal Risk Contribution (Risk Parity) long-only weights summing to 1.

    References:
        Maillard, S., Roncalli, T., & Teïletche, J. (2010). The Properties of
        Equally Weighted Risk Contribution Portfolios. The Journal of Portfolio
        Management, 36(4), 60-70. https://doi.org/10.3905/jpm.2010.36.4.060
        Spinu, F. (2013). An Algorithm for Computing Risk Parity Weights.
        SSRN 2297383. https://ssrn.com/abstract=2297383

    Equations:
        Unconstrained convex log-barrier formulation (Spinu 2013) in u = ln(y):
            min_u  0.5 * y^T Sigma y - (1 / N) * sum(u_i),   y = exp(u),
            grad = y * (Sigma y) - (1 / N),   w* = y* / sum(y*).
        At optimality, w_i (Sigma w)_i / (w^T Sigma w) = 1 / N for all i.

    Project adaptation:
        Estimates covariance with Ledoit-Wolf (2004) shrinkage by default
        (`shrinkage=True`) or accepts a precomputed covariance matrix
        (`is_cov=True`); when `max_weight < 1.0` binds, refines on the capped
        simplex by minimizing squared risk-contribution deviations via SLSQP.
    """
    from scipy.optimize import minimize

    if (
        not isinstance(max_weight, numbers.Real)
        or isinstance(max_weight, bool)
        or not math.isfinite(max_weight)
        or float(max_weight) <= 0.0
    ):
        raise ValueError("max_weight must be a positive finite number")
    cap = float(max_weight)
    names = None
    if is_cov:
        cov = np.asarray(returns, dtype=float)
        if cov.ndim != 2 or cov.shape[0] != cov.shape[1]:
            raise ValueError("covariance matrix must be a square 2D array")
        n_assets = cov.shape[0]
        if n_assets == 0:
            return np.empty(0, dtype=float)
        if cap * n_assets < 1.0 - 1e-12:
            raise ValueError("max_weight * number of assets must be at least 1.0")
        if n_assets == 1:
            return np.array([1.0], dtype=float)
        cov = 0.5 * (cov + cov.T)
    else:
        if isinstance(returns, dict):
            names = list(returns.keys())
            if not names:
                return {}
            arr = np.column_stack([np.asarray(returns[name], dtype=float) for name in names])
        else:
            arr = np.asarray(returns, dtype=float)
            if arr.ndim != 2:
                raise ValueError("returns must be a {name: series} dict or 2D (T, N) array")
        n_assets = arr.shape[1]
        if n_assets == 0:
            return {} if names is not None else np.empty(0, dtype=float)
        if cap * n_assets < 1.0 - 1e-12:
            raise ValueError("max_weight * number of assets must be at least 1.0")
        if n_assets == 1:
            return {names[0]: 1.0} if names is not None else np.array([1.0], dtype=float)
        finite_rows = arr[np.all(np.isfinite(arr), axis=1)]
        if len(finite_rows) < 2:
            eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
            return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq
        cov = ledoit_wolf_cov(finite_rows) if shrinkage else np.cov(finite_rows, rowvar=False, ddof=1)
        cov = 0.5 * (np.asarray(cov, dtype=float) + np.asarray(cov, dtype=float).T)

    diag = np.diag(cov)
    if not np.all(np.isfinite(cov)) or np.any(diag <= 1e-18):
        eq = np.full(n_assets, 1.0 / n_assets, dtype=float)
        return {name: float(eq[i]) for i, name in enumerate(names)} if names is not None else eq

    eig_min = float(np.min(np.linalg.eigvalsh(cov)))
    if eig_min <= 1e-12:
        cov = cov + (1e-12 - eig_min) * np.eye(n_assets, dtype=float)
    scale = float(np.trace(cov)) / float(n_assets)
    cov_s = cov / scale

    inv_vol = 1.0 / np.sqrt(np.diag(cov_s))
    y0 = inv_vol / math.sqrt(float(inv_vol @ cov_s @ inv_vol))
    u0 = np.log(np.maximum(y0, 1e-12))
    target_rc = 1.0 / float(n_assets)

    def spinu_obj(u: np.ndarray) -> tuple[float, np.ndarray]:
        uc = np.clip(u, -30.0, 30.0)
        y = np.exp(uc)
        sy = cov_s @ y
        val = 0.5 * float(y @ sy) - target_rc * float(np.sum(uc))
        grad = y * sy - target_rc
        return val, grad

    res = minimize(spinu_obj, u0, method="L-BFGS-B", jac=True, options={"ftol": 1e-15, "gtol": 1e-12})
    y_opt = np.exp(np.clip(res.x, -30.0, 30.0))
    weights = y_opt / float(np.sum(y_opt))

    if cap < 1.0 - 1e-12 and float(np.max(weights)) > cap + 1e-9:
        w0 = _project_capped_simplex(weights, cap)

        def rc_obj(w: np.ndarray) -> tuple[float, np.ndarray]:
            sw = cov_s @ w
            port_var = float(w @ sw)
            if port_var <= 1e-24:
                return 0.0, np.zeros_like(w)
            rc = (w * sw) / port_var
            diff = rc - target_rc
            val = float(np.sum(diff * diff))
            # d(rc_i)/d(w_j) = (delta_ij (Sigma w)_i + w_i Sigma_ij) / V - 2 rc_i (Sigma w)_j / V.
            grad = (2.0 / port_var) * (diff * sw + cov_s @ (diff * w) - 2.0 * float(diff @ rc) * sw)
            return val, grad

        capped = minimize(
            rc_obj,
            w0,
            method="SLSQP",
            jac=True,
            bounds=[(1e-9, cap)] * n_assets,
            constraints=[{"type": "eq", "fun": lambda w: float(np.sum(w)) - 1.0, "jac": lambda w: np.ones(n_assets)}],
            options={"ftol": 1e-14, "maxiter": 300},
        )
        if capped.success and np.all(np.isfinite(capped.x)):
            weights = _project_capped_simplex(capped.x, cap)
        else:
            weights = w0

    if names is not None:
        return {name: float(weights[i]) for i, name in enumerate(names)}
    return weights


def mvo_weights(
    expected_returns,
    cov,
    *,
    previous_weights=None,
    risk_aversion: float = 1.0,
    turnover_cost: float = 0.0,
    gross_limit: float = 1.0,
    net_limit: float | None = None,
    max_weight: float = 0.25,
    long_only: bool = False,
    sectors: dict | None = None,
    max_sector_gross: float = 0.0,
):
    """Turnover-aware constrained Mean-Variance portfolio optimizer (convex QP).

    References:
        Markowitz, H. (1952). Portfolio Selection. The Journal of Finance,
        7(1), 77-91. https://doi.org/10.2307/2975974
        Boyd, S., Busseti, E., Diamond, S., Kahn, R. N., Koh, K., Nystrup, P.,
        & Speth, J. (2017). Multi-Period Trading via Convex Optimization.
        Foundations and Trends in Optimization, 3(1), 1-76.
        https://doi.org/10.1561/2400000023

    Equations:
        min_w  0.5 * lambda * w^T Sigma w - mu^T w + c * ||w - w_prev||_1
        s.t.   |w_i| <= max_weight (w_i >= 0 if long_only),
               sum |w_i| <= gross_limit,  |sum w_i| <= net_limit,
               sum_{i in sector} |w_i| <= max_sector_gross.

    Project adaptation:
        Uses smooth nonnegative split variables w = w+ - w- (and trade splits
        w - w_prev = u+ - u- when turnover_cost > 0) solved by SLSQP with exact
        analytical gradients and Jacobians; outputs are quantized toward zero
        to WEIGHT_STEP so wire precision never exceeds exposure limits.
    """
    from scipy.optimize import minimize

    for label, value, allow_zero in (
        ("risk_aversion", risk_aversion, False),
        ("turnover_cost", turnover_cost, True),
        ("gross_limit", gross_limit, False),
        ("max_weight", max_weight, False),
        ("max_sector_gross", max_sector_gross, True),
    ):
        if (
            not isinstance(value, numbers.Real)
            or isinstance(value, bool)
            or not math.isfinite(value)
            or (float(value) < 0.0 if allow_zero else float(value) <= 0.0)
        ):
            raise ValueError(f"{label} must be a {'nonnegative' if allow_zero else 'positive'} finite number")
    if net_limit is not None and (
        not isinstance(net_limit, numbers.Real)
        or isinstance(net_limit, bool)
        or not math.isfinite(net_limit)
        or float(net_limit) < 0.0
    ):
        raise ValueError("net_limit must be None or a nonnegative finite number")

    names = None
    if isinstance(expected_returns, dict):
        names = list(expected_returns.keys())
        if not names:
            return {}
        mu_raw = np.array(
            [
                float(expected_returns[s])
                if isinstance(expected_returns[s], numbers.Real) and not isinstance(expected_returns[s], bool)
                else math.nan
                for s in names
            ],
            dtype=float,
        )
    else:
        mu_raw = np.asarray(expected_returns, dtype=float)
        if mu_raw.ndim != 1:
            raise ValueError("expected_returns must be a {symbol: return} dict or 1D array")
    n = len(mu_raw)
    if n == 0:
        return {} if names is not None else np.empty(0, dtype=float)

    cov_arr = np.asarray(cov, dtype=float)
    if cov_arr.shape != (n, n) or not np.all(np.isfinite(cov_arr)):
        raise ValueError(f"cov must be a finite ({n}, {n}) matrix")
    cov_arr = 0.5 * (cov_arr + cov_arr.T)
    eig_min = float(np.min(np.linalg.eigvalsh(cov_arr)))
    if eig_min < 0.0:
        cov_arr = cov_arr + (-eig_min + 1e-12) * np.eye(n, dtype=float)

    if previous_weights is None:
        w_prev = np.zeros(n, dtype=float)
    elif isinstance(previous_weights, dict):
        if names is None:
            raise ValueError("previous_weights dict requires expected_returns dict")
        w_prev = np.array([float(previous_weights.get(s, 0.0)) for s in names], dtype=float)
    else:
        w_prev = np.asarray(previous_weights, dtype=float)
        if w_prev.shape != (n,):
            raise ValueError(f"previous_weights must have length {n}")
    if not np.all(np.isfinite(w_prev)):
        raise ValueError("previous_weights must be finite")

    valid_mu = np.isfinite(mu_raw)
    mu = np.where(valid_mu, mu_raw, 0.0)
    lam = float(risk_aversion)
    tc = float(turnover_cost)
    g_lim = float(gross_limit)
    w_cap = float(max_weight)
    sec_cap = float(max_sector_gross)
    eps_tie = 1e-10

    sector_groups: list[list[int]] = []
    if sec_cap > 0.0:
        groups: dict[str, list[int]] = {}
        for i in range(n):
            if names is not None:
                sym = names[i]
                sec = sectors[sym] if sectors and sym in sectors else (product_sector(sym) or "other")
            else:
                sec = str(sectors[i]) if sectors and i in sectors else "other"
            groups.setdefault(sec, []).append(i)
        sector_groups = list(groups.values())

    n_vars = 4 * n if tc > 0.0 else 2 * n
    bounds = []
    for i in range(n):
        hi = w_cap if valid_mu[i] else 0.0
        bounds.append((0.0, hi))
    for i in range(n):
        hi = 0.0 if (long_only or not valid_mu[i]) else w_cap
        bounds.append((0.0, hi))
    if tc > 0.0:
        bounds.extend([(0.0, None)] * (2 * n))

    def objective(z: np.ndarray) -> tuple[float, np.ndarray]:
        wp = z[:n]
        wm = z[n : 2 * n]
        w = wp - wm
        cw = cov_arr @ w
        val = 0.5 * lam * float(w @ cw) - float(mu @ w) + eps_tie * float(np.sum(wp) + np.sum(wm))
        gw = lam * cw - mu
        if tc > 0.0:
            up = z[2 * n : 3 * n]
            um = z[3 * n : 4 * n]
            val += tc * float(np.sum(up) + np.sum(um))
            grad = np.concatenate([gw + eps_tie, -gw + eps_tie, np.full(2 * n, tc)])
        else:
            grad = np.concatenate([gw + eps_tie, -gw + eps_tie])
        return val, grad

    constraints = []
    # Gross exposure: sum(w+ + w-) <= gross_limit
    jac_gross = np.zeros(n_vars, dtype=float)
    jac_gross[: 2 * n] = -1.0
    constraints.append(
        {
            "type": "ineq",
            "fun": lambda z: g_lim - float(np.sum(z[: 2 * n])),
            "jac": lambda _z, j=jac_gross: j,
        }
    )

    if net_limit is not None:
        n_lim = float(net_limit)
        jac_net_hi = np.zeros(n_vars, dtype=float)
        jac_net_hi[:n] = -1.0
        jac_net_hi[n : 2 * n] = 1.0
        jac_net_lo = -jac_net_hi
        constraints.append(
            {
                "type": "ineq",
                "fun": lambda z, lim=n_lim: lim - float(np.sum(z[:n]) - np.sum(z[n : 2 * n])),
                "jac": lambda _z, j=jac_net_hi: j,
            }
        )
        constraints.append(
            {
                "type": "ineq",
                "fun": lambda z, lim=n_lim: lim + float(np.sum(z[:n]) - np.sum(z[n : 2 * n])),
                "jac": lambda _z, j=jac_net_lo: j,
            }
        )

    for idxs in sector_groups:
        idx_arr = np.array(idxs, dtype=int)
        jac_sec = np.zeros(n_vars, dtype=float)
        jac_sec[idx_arr] = -1.0
        jac_sec[n + idx_arr] = -1.0
        constraints.append(
            {
                "type": "ineq",
                "fun": lambda z, ia=idx_arr: sec_cap - float(np.sum(z[ia]) + np.sum(z[n + ia])),
                "jac": lambda _z, j=jac_sec: j,
            }
        )

    if tc > 0.0:
        eye_n = np.eye(n, dtype=float)
        jac_trade = np.hstack([eye_n, -eye_n, -eye_n, eye_n])
        constraints.append(
            {
                "type": "eq",
                "fun": lambda z: (z[:n] - z[n : 2 * n]) - (z[2 * n : 3 * n] - z[3 * n : 4 * n]) - w_prev,
                "jac": lambda _z, j=jac_trade: j,
            }
        )

    z0 = np.zeros(n_vars, dtype=float)
    if tc > 0.0:
        # w = 0 is always feasible: u_minus = max(0, w_prev), u_plus = max(0, -w_prev).
        z0[2 * n : 3 * n] = np.maximum(0.0, -w_prev)
        z0[3 * n : 4 * n] = np.maximum(0.0, w_prev)

    sol = minimize(
        objective,
        z0,
        method="SLSQP",
        jac=True,
        bounds=bounds,
        constraints=constraints,
        options={"ftol": 1e-12, "maxiter": 400},
    )
    z_opt = sol.x if np.all(np.isfinite(sol.x)) else z0
    w_opt = z_opt[:n] - z_opt[n : 2 * n]
    w_opt = np.where(np.abs(w_opt) < 1e-10, 0.0, w_opt)
    w_quant = np.array(
        [float(Decimal(str(val)).quantize(WEIGHT_STEP, rounding=ROUND_DOWN)) for val in w_opt],
        dtype=float,
    )
    if names is not None:
        return {name: float(w_quant[i]) for i, name in enumerate(names)}
    return w_quant
