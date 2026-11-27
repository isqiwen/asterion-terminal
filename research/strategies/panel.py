"""Rolling pooled futures forecasts on completed shared observation times."""
import numpy as np

from asterion_strategy import random_fourier_features, ridge_predict

from factors.liquidity import chaikin_money_flow
from factors.positions import interest_change
from factors.price import information_discreteness, momentum, volatility
from factors.risk import maximum_return
from factors.technical import efficiency_ratio, price_zscore, rsi
from factors.term_structure import roll_yield_carry
from strategies.risk_premia import _Ranked


# Gu, Kelly & Xiu (2020), "Empirical Asset Pricing via Machine Learning", Eqs. (4), (7), (8): https://doi.org/10.1093/rfs/hhaa009
# Adaptation: rolling pooled futures price/volume/OI features, optional carry and pure ridge with a shared intercept.
class PanelRidgeRank(_Ranked):
    """Pool trailing labeled observations across futures, then rank their shared-model forecasts.

    Ten bar features, optionally carry, predict next-open to next-rebalance-open returns.
    """
    params = {**_Ranked.params, "fast": 5, "train": 252, "ridge": 1.0, "use_carry": False}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if not 2 <= p["fast"] < p["window"] or p["train"] < 2 or p["ridge"] <= 0:
            raise ValueError("invalid panel forecast parameters")
        self.need = max(p["window"] + p["train"] + p["rebalance"] + 1, p["volatility"] + 1)

    def transform(self, train, query):
        return train, query

    def history(self, ctx, symbol, indices):
        data = super().history(ctx, symbol, indices)
        # Execution uses each symbol's first bar after a signal, even before the next shared observation.
        data["next_open"] = np.full(len(indices), np.nan)
        data["next_open"][:-1] = ctx.history(symbol, "open", 1 << 30)[np.asarray(indices[:-1]) + 1]
        return data

    def scores(self, data):
        p = self.params
        fast, horizon, count, window = p["fast"], p["rebalance"], p["train"], p["window"]
        training, labels, queries, symbols = [], [], [], []
        for symbol in sorted(data):
            row = data[symbol]
            close = row["close"]
            inputs = np.column_stack((
                momentum(close, fast), momentum(close, window), volatility(close, fast),
                price_zscore(close, window), rsi(close, fast), efficiency_ratio(close, window),
                information_discreteness(close, window), maximum_return(close, window),
                chaikin_money_flow(row["high"], row["low"], close, row["volume"], window),
                interest_change(row["open_interest"], window),
            ))
            if p["use_carry"]:
                inputs = np.column_stack((inputs, roll_yield_carry(row["roll_yield"], fast)))
            train = inputs[-horizon - count - 1:-horizon - 1]
            # The last label exits after the previous shared bar, so its execution price is already known.
            returns = row["next_open"][-count - 1:-1] / row["next_open"][-count - horizon - 1:-horizon - 1] - 1
            valid = np.isfinite(train).all(axis=1) & np.isfinite(returns)
            training.extend(train[valid])
            labels.extend(returns[valid])
            if np.isfinite(inputs[-1]).all():
                queries.append(inputs[-1])
                symbols.append(symbol)
        if len(training) < 2 or not queries:
            return {}
        prediction = ridge_predict(np.asarray(training), np.asarray(queries), np.asarray(labels),
                                   p["ridge"], self.transform)
        return dict(zip(symbols, prediction))


# Kelly, Malamud & Zhou (2024), "The Virtue of Complexity in Return Prediction", Eq. (20): https://doi.org/10.1111/jofi.13298
# Adaptation: the same pooled futures panel and labels as PanelRidgeRank, with fixed Gaussian sin/cos features.
class PanelRandomFeatureRank(PanelRidgeRank):
    """Shared random Fourier feature ridge model, with a fixed seed and training-only scales."""
    params = {**PanelRidgeRank.params, "features": 128, "gamma": 2.0, "seed": 7}

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["features"] < 2 or p["features"] % 2 or p["gamma"] <= 0 or p["seed"] < 0:
            raise ValueError("features must be positive and even, gamma positive, seed nonnegative")
        self.projection = np.random.default_rng(p["seed"]).normal(size=(p["features"] // 2, 10 + p["use_carry"])).T

    def transform(self, train, query):
        return random_fourier_features(train, query, self.projection, self.params["gamma"])
