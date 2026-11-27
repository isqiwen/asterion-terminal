"""Rolling training-window ridge regression and random Fourier feature transforms."""
import math
import numbers

import numpy as np
from scipy.linalg import solve


def ridge_predict(train, query, labels, ridge: float = 1.0, transform=None) -> np.ndarray:
    """Fit L2-regularized linear regression on `train`/`labels` and predict `query`.

    Standardizes inputs using training statistics only, applies optional
    `transform(train_std, query_std)`, centers transformed features, keeps an
    unpenalized intercept, and solves primal (`p <= n`) or dual (`p > n`) form.
    """
    if (
        not isinstance(ridge, numbers.Real)
        or isinstance(ridge, bool)
        or not math.isfinite(ridge)
        or float(ridge) <= 0.0
    ):
        raise ValueError("ridge must be a positive finite number")
    x_train = np.asarray(train, dtype=float)
    x_query = np.asarray(query, dtype=float)
    y_train = np.asarray(labels, dtype=float)
    mean = x_train.mean(axis=0)
    sigma = x_train.std(axis=0)
    sigma = np.where(sigma > 0.0, sigma, 1.0)
    x_train = (x_train - mean) / sigma
    x_query = (x_query - mean) / sigma
    if transform is not None:
        x_train, x_query = transform(x_train, x_query)
    center = x_train.mean(axis=0)
    x_train = x_train - center
    x_query = x_query - center
    intercept = float(y_train.mean())
    penalty = float(ridge) * len(x_train)
    if x_train.shape[1] <= len(x_train):
        gram = x_train.T @ x_train + penalty * np.eye(x_train.shape[1])
        beta = solve(gram, x_train.T @ (y_train - intercept), assume_a="pos")
        prediction = x_query @ beta
    else:
        gram = x_train @ x_train.T + penalty * np.eye(len(x_train))
        dual = solve(gram, y_train - intercept, assume_a="pos")
        prediction = (x_query @ x_train.T) @ dual
    return prediction + intercept


def random_fourier_features(
    train,
    query,
    projection,
    gamma: float = 1.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Paired sin/cos random Fourier features standardized on the training window.

    References:
        Rahimi, A., & Recht, B. (2007). Random Features for Large-Scale Kernel
        Machines. NeurIPS 20.
        https://papers.nips.cc/paper/2007/hash/013a006f03dbc5392effeb8f18fda755-Abstract.html
        Kelly, B., Malamud, S., & Zhou, K. (2024). The Virtue of Complexity in
        Return Prediction. The Journal of Finance, 79(1), 459-503, Eq. (20).
        https://doi.org/10.1111/jofi.13298
    """
    proj = np.asarray(projection, dtype=float)
    scale = float(gamma)
    t_proj = scale * (np.asarray(train, dtype=float) @ proj)
    q_proj = scale * (np.asarray(query, dtype=float) @ proj)
    t_feat = np.column_stack((np.sin(t_proj), np.cos(t_proj)))
    q_feat = np.column_stack((np.sin(q_proj), np.cos(q_proj)))
    sigma = t_feat.std(axis=0)
    sigma = np.where(sigma > 0.0, sigma, 1.0)
    return t_feat / sigma, q_feat / sigma
