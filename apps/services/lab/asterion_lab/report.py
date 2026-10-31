"""QuantStats figures of a finished run, from its daily settlements; asked for
when the result page shows them, in a child process, so that a backtest or a
sweep never loads QuantStats:

    python3 -I -B -c "...; from asterion_lab.report import main; main()" <lab-directory> <run>

Returns are each settlement's equity against the one before it (the first
against the deposit), subtracted in exact decimals before they become floating
point. A ratio without a value (no losing day for Sortino or Omega) is left
out rather than shown as infinite.
"""
from datetime import timedelta
from decimal import Decimal
import json
import math
import os
from pathlib import Path
import sys
import warnings

from asterion_lab.runs import Runs

MINIMUM_DAYS = 20


def returns_of(deposit: Decimal, days: list, equities: list):
    """Daily returns and equity as a share of the deposit."""
    import pandas as pd

    if len(days) < MINIMUM_DAYS:
        raise ValueError(f"QuantStats figures need at least {MINIMUM_DAYS} settled days")
    previous, values = deposit, []
    for equity in equities:
        if equity <= 0:
            raise ValueError("QuantStats figures need positive equity")
        values.append(float(equity - previous) / float(previous))
        previous = equity
    index = pd.to_datetime([day.isoformat() for day in days])
    shares = pd.Series([1.0, *(float(equity / deposit) for equity in equities)],
                       index=pd.to_datetime([(days[0] - timedelta(days=1)).isoformat(),
                                             *(day.isoformat() for day in days)]))
    return pd.Series(values, index=index, dtype=float), shares


def figures(deposit: Decimal, days: list, equities: list) -> dict:
    import quantstats as qs

    returns, shares = returns_of(deposit, days, equities)
    losses = bool((returns < 0).any())
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        values = {
            "sortino": qs.stats.sortino(returns, rf=0, periods=252) if losses else math.nan,
            "omega": qs.stats.omega(returns, rf=0, required_return=0, periods=252)
            if losses else math.nan,
            "tail_ratio": qs.stats.tail_ratio(returns, cutoff=0.95, prepare_returns=False),
            # The deposit belongs in the drawdown's baseline.
            "ulcer_index": qs.stats.ulcer_index(shares),
            "value_at_risk": qs.stats.value_at_risk(returns, confidence=0.95, prepare_returns=False),
            "conditional_value_at_risk": qs.stats.conditional_value_at_risk(
                returns, confidence=0.95, prepare_returns=False),
        }
    return {"version": qs.__version__,
            "figures": {name: float(value) for name, value in values.items()
                        if math.isfinite(value)}}


def run(directory: Path, identity: str) -> dict:
    runs = Runs(directory)
    record = runs.record(identity)
    settlements = runs.table(identity, "settlements")
    return figures(Decimal(record["costs"]["deposit"]), settlements["trading_day"].to_list(),
                   settlements["equity"].to_list())


def main():
    # QuantStats and its libraries print; standard output carries the reply.
    output = os.fdopen(os.dup(1), "w")
    os.dup2(2, 1)
    try:
        result = run(Path(sys.argv[1]), sys.argv[2])
    except (ValueError, KeyError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
    output.write(json.dumps(result))
    output.flush()
