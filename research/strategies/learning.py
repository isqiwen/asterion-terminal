"""Rolling return forecasts using linear and random Fourier feature ridge regression."""
import numpy as np
from scipy.linalg import solve

from asterion_strategy import Strategy

from factors.price import momentum, volatility
from factors.technical import efficiency_ratio, price_zscore, rsi
from strategies._common import SIDES, lots


# Linear control for RandomFeatureForecast, with the same inputs, labels and rolling fit.
class RidgeForecast(Strategy):
    """Linear ridge on six price features, predicting next-open to next-rebalance-open returns.

    Training-window standardization and an unpenalized intercept are fitted again at each rebalance.
    """
    params = {"fast": 5, "slow": 60, "train": 252, "ridge": 1.0, "rebalance": 5,
              "threshold": 0.0, "lots": 1, "sides": "both"}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if not 2 <= p["fast"] < p["slow"] or p["train"] < 2 or p["ridge"] <= 0 \
                or p["rebalance"] < 1 or p["threshold"] < 0 or p["lots"] < 1 or p["sides"] not in SIDES:
            raise ValueError("invalid forecast parameters")
        self.need = p["slow"] + p["train"] + p["rebalance"] + 1
        self.steps = {}

    def ready(self, ctx):
        return all(len(ctx.history(s, "close", self.need)) == self.need for s in ctx.symbols)

    def transform(self, train, query):
        return train, query

    def forecast(self, open, close):
        p = self.params
        horizon = p["rebalance"]
        inputs = np.column_stack((momentum(close, p["fast"]), momentum(close, p["slow"]),
                                  volatility(close, p["fast"]), price_zscore(close, p["slow"]),
                                  rsi(close, p["fast"]), efficiency_ratio(close, p["slow"])))
        train = inputs[p["slow"]:-horizon - 1]
        query = inputs[-1:]
        # Targets execute at the next open; exclude features whose holding-period return is not yet known.
        labels = open[p["slow"] + horizon + 1:] / open[p["slow"] + 1:-horizon] - 1
        if not np.isfinite(train).all() or not np.isfinite(query).all() or not np.isfinite(labels).all():
            return np.nan
        mean, sigma = train.mean(axis=0), train.std(axis=0)
        sigma = np.where(sigma > 0, sigma, 1.0)
        train, query = self.transform((train - mean) / sigma, (query - mean) / sigma)
        center = train.mean(axis=0)
        train, query = train - center, query - center
        intercept = labels.mean()
        penalty = p["ridge"] * len(train)
        if train.shape[1] <= len(train):
            gram = train.T @ train + penalty * np.eye(train.shape[1])
            beta = solve(gram, train.T @ (labels - intercept), assume_a="pos")
            prediction = query @ beta
        else:
            gram = train @ train.T + penalty * np.eye(len(train))
            dual = solve(gram, labels - intercept, assume_a="pos")
            prediction = (query @ train.T) @ dual
        return float(prediction[0] + intercept)

    def on_bar(self, ctx, bar):
        p = self.params
        seen = self.steps.get(bar.symbol, 0) + 1
        self.steps[bar.symbol] = seen
        if seen < self.need or (seen - self.need) % p["rebalance"]:
            return
        value = self.forecast(ctx.history(bar.symbol, "open", self.need),
                              ctx.history(bar.symbol, "close", self.need))
        side = int(value > p["threshold"]) - int(value < -p["threshold"]) if np.isfinite(value) else 0
        ctx.target(bar.symbol, lots(side, p))


# Kelly, Malamud & Zhou (2024), "The Virtue of Complexity in Return Prediction", Eq. (20): https://doi.org/10.1111/jofi.13298
# Adaptation: six futures-price features, next-open holding returns and an unpenalized intercept.
class RandomFeatureForecast(RidgeForecast):
    """Random Fourier features + ridge (Kelly-Malamud-Zhou, JF 2024), adapted to futures prices.

    Uses paired sin/cos features and a fixed seed; features=128 means 64 random projections.
    """
    params = {**RidgeForecast.params, "features": 128, "gamma": 2.0, "seed": 7}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["features"] < 2 or p["features"] % 2 or p["gamma"] <= 0 or p["seed"] < 0:
            raise ValueError("features must be positive and even, gamma positive, seed nonnegative")
        self.projection = np.random.default_rng(p["seed"]).normal(size=(p["features"] // 2, 6)).T

    def transform(self, train, query):
        train = self.params["gamma"] * (train @ self.projection)
        query = self.params["gamma"] * (query @ self.projection)
        train = np.column_stack((np.sin(train), np.cos(train)))
        query = np.column_stack((np.sin(query), np.cos(query)))
        sigma = train.std(axis=0)
        sigma = np.where(sigma > 0, sigma, 1.0)
        return train / sigma, query / sigma
