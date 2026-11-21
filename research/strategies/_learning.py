"""Shared training-only normalization and ridge fitting for rolling forecasts."""
import numpy as np
from scipy.linalg import solve

from factors.price import momentum, volatility
from factors.technical import efficiency_ratio, price_zscore, rsi


def price_features(close, fast, slow):
    return np.column_stack((momentum(close, fast), momentum(close, slow), volatility(close, fast),
                            price_zscore(close, slow), rsi(close, fast), efficiency_ratio(close, slow)))


def ridge_predict(train, query, labels, ridge, transform):
    mean, sigma = train.mean(axis=0), train.std(axis=0)
    sigma = np.where(sigma > 0, sigma, 1.0)
    train, query = transform((train - mean) / sigma, (query - mean) / sigma)
    center = train.mean(axis=0)
    train, query = train - center, query - center
    intercept = labels.mean()
    penalty = ridge * len(train)
    if train.shape[1] <= len(train):
        gram = train.T @ train + penalty * np.eye(train.shape[1])
        beta = solve(gram, train.T @ (labels - intercept), assume_a="pos")
        prediction = query @ beta
    else:
        gram = train @ train.T + penalty * np.eye(len(train))
        dual = solve(gram, labels - intercept, assume_a="pos")
        prediction = (query @ train.T) @ dual
    return prediction + intercept


# Kelly, Malamud & Zhou (2024), "The Virtue of Complexity in Return Prediction", Eq. (20): https://doi.org/10.1111/jofi.13298
def fourier(train, query, projection, gamma):
    train, query = gamma * (train @ projection), gamma * (query @ projection)
    train = np.column_stack((np.sin(train), np.cos(train)))
    query = np.column_stack((np.sin(query), np.cos(query)))
    sigma = train.std(axis=0)
    sigma = np.where(sigma > 0, sigma, 1.0)
    return train / sigma, query / sigma
