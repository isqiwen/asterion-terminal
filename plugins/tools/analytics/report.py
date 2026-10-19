"""Offline analysis of Task-verified backtests; no market-data acquisition."""
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation
from pathlib import Path
import math
import shutil
import socket
import struct
import time
import sys
import warnings

import matplotlib
matplotlib.use("Agg")
if getattr(sys, "frozen", False):
    # The prepared cache contains only Matplotlib's bundled, relative font paths.
    # Seed PyInstaller's private config directory before QuantStats imports plots.
    for cache in (Path(__file__).parent / "matplotlib-cache").iterdir():
        shutil.copyfile(cache, Path(matplotlib.get_cachedir()) / cache.name)

import numpy as np
import pandas as pd
import polars as pl
import quantstats as qs
import skfolio
from cvxpy.error import SolverError
from skfolio import RiskMeasure
from skfolio.optimization import EqualWeighted, MeanRisk, ObjectiveFunction
from skfolio.prior import EmpiricalPrior
from skfolio.moments import LedoitWolf
from asterion.v1.analytics_pb2 import (
    BacktestEquity, AnalyticsRequest, AnalyticsReply, QuantStatsReport, PortfolioReport,
)


def settlement_returns(request: BacktestEquity) -> pl.DataFrame:
    if not 20 <= len(request.settlements) <= 200000:
        raise ValueError("Analytics requires 20 to 200000 settlement observations")
    deposit = Decimal(request.deposit)
    if not deposit.is_finite() or deposit <= 0:
        raise ValueError("Analytics requires positive finite equity")
    days, equities = [], []
    for row in request.settlements:
        day = date.fromisoformat(row.trading_day)
        if day.isoformat() != row.trading_day or (days and row.trading_day <= days[-1]):
            raise ValueError("Analytics settlement dates must ascend")
        equity = Decimal(row.equity)
        if not equity.is_finite() or equity <= 0:
            raise ValueError("Analytics requires positive finite equity")
        days.append(row.trading_day)
        equities.append(equity)
    # The C++ ledger supplies fixed-scale Decimal text. Subtract before any
    # floating conversion so small P&L on a large balance is not rounded away.
    frame = pl.DataFrame({"trading_day": days, "equity": equities},
                         schema={"trading_day": pl.String, "equity": pl.Decimal(38, 8)})
    frame = frame.with_columns(
        pl.col("equity").shift(1).fill_null(deposit).alias("previous"),
        pl.col("trading_day").shift(1).fill_null("").alias("previous_day"),
    ).with_columns(
        ((pl.col("equity") - pl.col("previous")).cast(pl.Float64)
         / pl.col("previous").cast(pl.Float64)).alias("return"),
        (pl.col("equity").cast(pl.Float64) / float(deposit)).alias("normalized_equity"),
    )
    if not frame.select(pl.all_horizontal(pl.col("return", "normalized_equity").is_finite()).all()).item():
        raise ValueError("Analytics requires finite daily returns")
    return frame


def quantstats_report(request: BacktestEquity) -> QuantStatsReport:
    frame = settlement_returns(request)
    days, values = frame["trading_day"].to_list(), frame["return"].to_list()
    returns = pd.Series(values, index=pd.to_datetime(days), dtype=float)
    result = QuantStatsReport(task_id=request.task_id, result_sha256=request.result_sha256,
                             library_version=qs.__version__, polars_version=pl.__version__, periods_per_year=252)
    for day, value in zip(days, values, strict=True):
        result.returns.add(trading_day=day, value=value)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # Undefined ratios stay absent.
        metrics = {
            "sortino": qs.stats.sortino(returns, rf=0, periods=252) if (returns < 0).any() else math.nan,
            "omega": qs.stats.omega(returns, rf=0, required_return=0, periods=252)
                if (returns < 0).any() else math.nan,
            "tail_ratio": qs.stats.tail_ratio(returns, cutoff=0.95, prepare_returns=False),
            # Initial capital belongs in the drawdown baseline. Maximum >= 1
            # explicitly makes this an equity series for QuantStats.
            "ulcer_index": qs.stats.ulcer_index(pd.Series([1.0, *frame["normalized_equity"].to_list()],
                index=pd.to_datetime([date.fromisoformat(days[0]) - timedelta(days=1), *days]))),
            "value_at_risk": qs.stats.value_at_risk(returns, confidence=0.95, prepare_returns=False),
            "conditional_value_at_risk": qs.stats.conditional_value_at_risk(
                returns, confidence=0.95, prepare_returns=False),
        }
    for name, value in metrics.items():
        if math.isfinite(value):
            setattr(result, name, value)
    return result


def portfolio_report(request) -> PortfolioReport:
    if not 2 <= len(request.backtests) <= 3 or len({v.task_id for v in request.backtests}) != len(request.backtests):
        raise ValueError("Portfolio analysis requires 2 or 3 distinct backtests")
    if date.fromisoformat(request.training_end).isoformat() != request.training_end:
        raise ValueError("Portfolio analysis requires a training end date")
    frames = [settlement_returns(v).select("previous_day", "trading_day", pl.col("return").alias(f"r{i}"))
              for i, v in enumerate(request.backtests)]
    aligned = frames[0]
    for frame in frames[1:]:
        # Matching only the ending day would mix returns spanning different
        # settlement intervals when one experiment has a missing observation.
        aligned = aligned.join(frame, on=["previous_day", "trading_day"], how="inner", validate="1:1")
    aligned = aligned.sort("trading_day")
    train = aligned.filter(pl.col("trading_day") <= request.training_end)
    validation = aligned.filter(pl.col("trading_day") > request.training_end)
    if min(train.height, validation.height) < 20:
        raise ValueError("Portfolio analysis requires 20 common training and 20 validation observations")
    columns = [f"r{i}" for i in range(len(frames))]
    x_train, x_test = train.select(columns).to_numpy(), validation.select(columns).to_numpy()
    if not (np.std(x_train, axis=0) > 0).all():
        raise ValueError("Portfolio training returns must vary for every backtest")
    model = MeanRisk(objective_function=ObjectiveFunction.MINIMIZE_RISK,
                     risk_measure=RiskMeasure.VARIANCE, min_weights=0, max_weights=1, budget=1,
                     prior_estimator=EmpiricalPrior(covariance_estimator=LedoitWolf()),
                     solver="CLARABEL", raise_on_failure=True)
    model.fit(x_train)
    weights = model.weights_
    # Convex solvers enforce constraints to numerical tolerance. Reject failed
    # solutions; do not change algorithm, clip weights or fit on validation data.
    if not np.isfinite(weights).all() or np.min(weights) < -1e-8 or np.max(weights) > 1 + 1e-8 or abs(sum(weights) - 1) > 1e-6:
        raise SolverError("invalid portfolio weights")
    optimized = model.predict(x_test).returns
    equal = EqualWeighted().fit(x_train).predict(x_test).returns
    output = pl.DataFrame({"trading_day": validation["trading_day"], "optimized": optimized, "equal": equal})
    summary = output.select(
        ((1 + pl.col("optimized")).product() - 1).alias("optimized_total"),
        ((1 + pl.col("equal")).product() - 1).alias("equal_total"),
        (pl.col("optimized").std(ddof=1) * math.sqrt(252)).alias("optimized_volatility"),
        (pl.col("equal").std(ddof=1) * math.sqrt(252)).alias("equal_volatility"),
    ).row(0)
    if not np.isfinite(optimized).all() or not np.isfinite(equal).all() or not all(math.isfinite(v) for v in summary):
        raise SolverError("non-finite portfolio report")
    report = PortfolioReport(polars_version=pl.__version__, skfolio_version=skfolio.__version__,
        training_end=request.training_end, training_begin=train["trading_day"][0],
        training_last=train["trading_day"][-1], training_observations=train.height,
        optimized_total_return=summary[0], equal_weight_total_return=summary[1],
        optimized_annual_volatility=summary[2], equal_weight_annual_volatility=summary[3])
    for source, weight in zip(request.backtests, weights, strict=True):
        report.weights.add(task_id=source.task_id, result_sha256=source.result_sha256,
                           weight=weight, source_observations=len(source.settlements))
    for day, optimized_return, equal_return in output.iter_rows():
        report.validation.add(trading_day=day, optimized_return=optimized_return, equal_weight_return=equal_return)
    return report


def main():
    # Native owns this one-request child and its private Unix Socket directory.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(sys.argv[1])
        listener.listen(1)
        listener.settimeout(30)
        connection, _ = listener.accept()
        with connection:
            deadline = time.monotonic() + 30
            def read(size):
                data = bytearray()
                while len(data) < size:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("Analytics timed out")
                    connection.settimeout(remaining)
                    chunk = connection.recv(size - len(data))
                    if not chunk:
                        raise EOFError("Analytics IPC closed")
                    data.extend(chunk)
                return data
            size, = struct.unpack("!I", read(4))
            if not 0 < size <= 64 * 1024 * 1024:
                raise ValueError("invalid IPC frame length")
            request = AnalyticsRequest.FromString(read(size))
            reply = AnalyticsReply()
            try:
                if request.HasField("quantstats"):
                    reply.quantstats.CopyFrom(quantstats_report(request.quantstats))
                elif request.HasField("portfolio"):
                    reply.portfolio.CopyFrom(portfolio_report(request.portfolio))
                else:
                    raise ValueError("Analytics request has no operation")
            except (ValueError, InvalidOperation) as error:
                reply.error.code = "invalid_request"
                reply.error.message = str(error)
            except SolverError:
                reply.error.code = "operation_failed"
                reply.error.message = "Portfolio optimization failed"
            data = reply.SerializeToString()
            connection.settimeout(max(0, deadline - time.monotonic()))
            connection.sendall(struct.pack("!I", len(data)) + data)


if __name__ == "__main__":
    main()
