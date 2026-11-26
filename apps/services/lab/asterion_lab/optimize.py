"""High-dimensional parameter search: Latin Hypercube Sampling and GP Bayesian Optimization.

Pure NumPy / SciPy implementation over discrete parameter grids, supporting
both one-shot space-filling exploration (LHS) and iterative wave-based
Gaussian Process Expected Improvement with Kriging Believer parallel batching.
"""
from collections.abc import Sequence
import itertools
import math

import numpy as np
from scipy.linalg import cho_factor, cho_solve, solve_triangular
from scipy.special import ndtr

DEFAULT_SEED = 20260401
SQRT5 = math.sqrt(5.0)
INV_SQRT2PI = 1.0 / math.sqrt(2.0 * math.pi)
EXACT_POOL_LIMIT = 10_000
SAMPLED_POOL_SIZE = 2048


def encode_axis(values: Sequence) -> list[float]:
    """Normalize discrete parameter values onto `[0.0, 1.0]` for GP distance calculation."""
    n = len(values)
    if n <= 1:
        return [0.0] * n
    numeric = []
    for val in values:
        if isinstance(val, bool):
            numeric.append(1.0 if val else 0.0)
        elif isinstance(val, (int, float)) and math.isfinite(float(val)):
            numeric.append(float(val))
        else:
            numeric = []
            break
    if len(numeric) == n:
        lo, hi = min(numeric), max(numeric)
        if hi > lo:
            return [(v - lo) / (hi - lo) for v in numeric]
    return [i / float(n - 1) for i in range(n)]


def lhs_sample(
    axis_lengths: Sequence[int],
    n_samples: int,
    *,
    exclude: set[tuple[int, ...]] | None = None,
    seed: int = DEFAULT_SEED,
) -> list[tuple[int, ...]]:
    """Stratified Latin Hypercube Sampling over a discrete Cartesian grid.

    Reference:
        McKay, M. D., Beckman, R. J., & Conover, W. J. (1979). A Comparison of
        Three Methods for Selecting Values of Input Variables in the Analysis of
        Output from a Computer Code. Technometrics, 21(2), 239-245.
        https://doi.org/10.1080/00401706.1979.10489755

    Project adaptation:
        Maps stratified uniform quantiles `((perm + U) / n_samples)` onto
        discrete axis indices, deduplicates collisions, and tops up missing
        unique tuples deterministically so the returned list has exact length
        `min(n_samples, prod(axis_lengths) - len(exclude))`.
    """
    dims = [int(length) for length in axis_lengths]
    if not dims or any(length < 1 for length in dims):
        raise ValueError("axis_lengths must contain positive integers")
    if n_samples < 1:
        return []
    total = math.prod(dims)
    blocked = exclude or set()
    target = min(int(n_samples), max(0, total - len(blocked)))
    if target == 0:
        return []

    rng = np.random.default_rng(int(seed))
    picked: list[tuple[int, ...]] = []
    seen = set(blocked)

    # First draw stratified Latin Hypercube strata across every axis.
    strata = np.empty((target, len(dims)), dtype=int)
    for d, length in enumerate(dims):
        perm = rng.permutation(target)
        u = rng.uniform(0.0, 1.0, size=target)
        q = (perm + u) / float(target)
        strata[:, d] = np.minimum(length - 1, (q * length).astype(int))
    for row in strata:
        combo = tuple(int(v) for v in row)
        if combo not in seen:
            seen.add(combo)
            picked.append(combo)
            if len(picked) == target:
                return picked

    # Fill any collisions from remaining grid points without enumerating huge grids.
    if total <= EXACT_POOL_LIMIT:
        remaining = [combo for combo in itertools.product(*(range(k) for k in dims)) if combo not in seen]
        if remaining:
            order = rng.permutation(len(remaining))
            for idx in order[: target - len(picked)]:
                combo = remaining[int(idx)]
                seen.add(combo)
                picked.append(combo)
        return picked

    attempts = 0
    max_attempts = max(1000, target * 20)
    while len(picked) < target and attempts < max_attempts:
        attempts += 1
        combo = tuple(int(rng.integers(0, length)) for length in dims)
        if combo not in seen:
            seen.add(combo)
            picked.append(combo)
    return picked


def _matern52(a: np.ndarray, b: np.ndarray, length_scale: float) -> np.ndarray:
    """Isotropic Matérn-5/2 covariance matrix between rows of `a` and `b`.

    Reference:
        Rasmussen, C. E., & Williams, C. K. I. (2006). Gaussian Processes for
        Machine Learning. MIT Press, Ch. 4, Eq. (4.17).
        https://gaussianprocess.org/gpml/
    """
    diff = a[:, None, :] - b[None, :, :]
    sq = np.sum(diff * diff, axis=2)
    r = np.sqrt(np.maximum(0.0, sq)) / max(length_scale, 1e-6)
    s = SQRT5 * r
    return (1.0 + s + (s * s) / 3.0) * np.exp(-s)


def _fit_gp(x_obs: np.ndarray, y_std: np.ndarray) -> tuple[float, float, tuple, np.ndarray]:
    """Select Matérn-5/2 length-scale and noise variance maximizing log marginal likelihood."""
    n = len(y_std)
    eye = np.eye(n, dtype=float)
    best_lml = -math.inf
    best = None
    for length_scale in (0.2, 0.5, 1.0, 2.0):
        k_base = _matern52(x_obs, x_obs, length_scale)
        for noise in (1e-4, 1e-2, 1e-1):
            k_mat = k_base + (noise + 1e-8) * eye
            try:
                factor = cho_factor(k_mat, lower=True, check_finite=False)
                alpha = cho_solve(factor, y_std, check_finite=False)
            except np.linalg.LinAlgError:
                continue
            log_det = 2.0 * float(np.sum(np.log(np.diag(factor[0]))))
            lml = -0.5 * float(y_std @ alpha) - 0.5 * log_det
            if math.isfinite(lml) and lml > best_lml:
                best_lml = lml
                best = (length_scale, noise, factor, alpha)
    if best is None:
        k_mat = _matern52(x_obs, x_obs, 0.5) + 0.1 * eye
        factor = cho_factor(k_mat, lower=True, check_finite=False)
        alpha = cho_solve(factor, y_std, check_finite=False)
        best = (0.5, 0.1, factor, alpha)
    return best


def _expected_improvement(mu: np.ndarray, sigma: np.ndarray, best_y: float, xi: float) -> np.ndarray:
    """Expected Improvement acquisition function (Jones, Schonlau & Welch 1998)."""
    imp = mu - best_y - xi
    safe_s = np.maximum(sigma, 1e-12)
    z = imp / safe_s
    pdf = INV_SQRT2PI * np.exp(-0.5 * np.minimum(z * z, 700.0))
    cdf = ndtr(z)
    ei = imp * cdf + safe_s * pdf
    return np.where(sigma > 1e-11, np.maximum(0.0, ei), 0.0)


def gp_suggest(
    axis_coords: Sequence[Sequence[float]],
    observed: Sequence[tuple[tuple[int, ...], float]],
    batch_size: int,
    *,
    tried: set[tuple[int, ...]] | None = None,
    seed: int = DEFAULT_SEED,
    xi: float = 0.01,
) -> list[tuple[int, ...]]:
    """Suggest the next batch of unevaluated parameter tuples via GP Expected Improvement.

    References:
        Jones, D. R., Schonlau, M., & Welch, W. J. (1998). Efficient Global
        Optimization of Expensive Black-Box Functions. Journal of Global
        Optimization, 13(4), 455-492. https://doi.org/10.1023/A:1008306431147
        Rasmussen, C. E., & Williams, C. K. I. (2006). Gaussian Processes for
        Machine Learning. MIT Press, Ch. 2 & 4. https://gaussianprocess.org/gpml/
        Ginsbourger, D., Le Riche, R., & Carraro, L. (2010). Kriging Is
        Well-Suited to Parallelize Optimization. Studies in Computational
        Intelligence, 131-162. https://doi.org/10.1007/978-3-642-10701-6_6

    Project adaptation:
        Operates over discrete normalized parameter grids; uses exact grid
        evaluation when the grid has <= 10,000 combinations and deterministic
        LHS candidate subsampling (2,048 points) on larger spaces. For
        `batch_size > 1`, applies Kriging Believer fantasy updates.
    """
    dims = [len(coords) for coords in axis_coords]
    if not dims or any(length < 1 for length in dims) or batch_size < 1:
        return []
    blocked = set(tried or ())
    for combo, _ in observed:
        blocked.add(tuple(combo))
    total = math.prod(dims)
    remaining_count = total - len(blocked)
    if remaining_count <= 0:
        return []
    want = min(int(batch_size), remaining_count)

    finite_obs = [
        (tuple(combo), float(score))
        for combo, score in observed
        if score is not None and math.isfinite(float(score))
    ]
    if len(finite_obs) < 2:
        return lhs_sample(dims, want, exclude=blocked, seed=seed)

    # Build candidate pool of unevaluated tuples.
    if total <= EXACT_POOL_LIMIT:
        pool = [combo for combo in itertools.product(*(range(k) for k in dims)) if combo not in blocked]
    else:
        pool = lhs_sample(dims, min(SAMPLED_POOL_SIZE, remaining_count), exclude=blocked, seed=seed)
    if not pool:
        return []

    def to_vec(combo: tuple[int, ...]) -> list[float]:
        return [float(axis_coords[d][idx]) for d, idx in enumerate(combo)]

    x_obs = np.array([to_vec(combo) for combo, _ in finite_obs], dtype=float)
    y_raw = np.array([score for _, score in finite_obs], dtype=float)
    y_mean = float(np.mean(y_raw))
    y_std_scale = float(np.std(y_raw, ddof=0))
    if y_std_scale <= 1e-12:
        return lhs_sample(dims, want, exclude=blocked, seed=seed)
    y_std = (y_raw - y_mean) / y_std_scale

    x_pool = np.array([to_vec(combo) for combo in pool], dtype=float)
    available = np.ones(len(pool), dtype=bool)
    suggestions: list[tuple[int, ...]] = []

    length_scale, noise, factor, alpha = _fit_gp(x_obs, y_std)
    work_x = x_obs
    work_y = y_std

    for step in range(want):
        if step > 0:
            k_mat = _matern52(work_x, work_x, length_scale) + (noise + 1e-8) * np.eye(len(work_x))
            try:
                factor = cho_factor(k_mat, lower=True, check_finite=False)
                alpha = cho_solve(factor, work_y, check_finite=False)
            except np.linalg.LinAlgError:
                break
        avail_idx = np.flatnonzero(available)
        if len(avail_idx) == 0:
            break
        x_cand = x_pool[avail_idx]
        k_star = _matern52(work_x, x_cand, length_scale)
        mu = k_star.T @ alpha
        v = solve_triangular(factor[0], k_star, lower=True, check_finite=False)
        var = np.maximum(1e-12, 1.0 - np.sum(v * v, axis=0))
        sigma = np.sqrt(var)
        ei = _expected_improvement(mu, sigma, float(np.max(work_y)), xi)
        best_local = int(np.argmax(ei))
        # If EI vanishes everywhere, explore the highest-uncertainty point.
        if ei[best_local] <= 0.0:
            best_local = int(np.argmax(sigma))
        pool_pos = int(avail_idx[best_local])
        available[pool_pos] = False
        chosen = pool[pool_pos]
        suggestions.append(chosen)
        # Kriging Believer fantasy observation for parallel batch diversity.
        work_x = np.vstack([work_x, x_pool[pool_pos:pool_pos + 1]])
        work_y = np.append(work_y, float(mu[best_local]))

    return suggestions
