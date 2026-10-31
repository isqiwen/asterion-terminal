"""Rolling validation of a sweep, by the old engine's rule (docs/backtest.md).

Every candidate of the sweep was replayed alone over all the days. A round
reads of those replays only its `training` days, before its first validation
day, and selects the candidate with the highest Sharpe ratio there, the first
among equals; with none it follows no one. Rounds move on by `validation`
days. One account then replays the days with every candidate reading every
bar, taking the targets of the round's choice: nothing before the first
round, and from each round's first day what its choice wanted after the day
before.

The service runs it in a child process once every candidate has succeeded:

    python3 -I -B -c "...; from asterion_lab.rolling import main; main()" <lab-directory>

reads its RollingRun (asterion/lab.proto) from standard input.
"""
import sys
from pathlib import Path

from asterion_lab import backtest, repository
from asterion_lab.backtest import R, raw
from asterion_lab.runs import Plan, Runs
from asterion_lab.strategy import Context, step
from asterion_lab.warehouse import Warehouse

MIN_TRAINING = 20
MAX_ROUNDS = 100


def rounds(days: int, training: int, validation: int) -> list:
    """Each round's first validation day and the day after its last, by index."""
    if training < MIN_TRAINING or validation < 1 or training >= days:
        raise ValueError(f"rolling validation needs at least {MIN_TRAINING} training days, "
                         "validation days after them and one validation day a round")
    result = [(first, min(days, first + validation)) for first in range(training, days, validation)]
    if len(result) > MAX_ROUNDS:
        raise ValueError(f"rolling validation has at most {MAX_ROUNDS} rounds, not {len(result)}")
    return result


class Curve:
    """A candidate's equity record by trading day."""

    def __init__(self, runs: Runs, identity: str):
        equity = runs.table(identity, "equity")
        self.marks = [backtest.raw(value) for value in equity["equity"]]
        self.first_ns = equity["time_ns"][0]
        self.day_end = [n for n, event in enumerate(equity["event"]) if event == "settlement"]
        settled = runs.table(identity, "settlements")
        self.days = [day.isoformat() for day in settled["trading_day"]]
        self.equity = [backtest.raw(value) for value in settled["equity"]]
        self.times = list(settled["time_ns"])

    def sharpe(self, begin: int, first: int, deposit: int, config):
        """The Sharpe ratio of training days [begin, first), from the equity of
        the day before them."""
        import asterion_engine

        base = self.equity[begin - 1] if begin else deposit
        if base <= 0:
            return None
        start = self.day_end[begin - 1] + 1 if begin else 0
        values = asterion_engine.performance(
            base, list(zip(self.days[begin:first], self.equity[begin:first])),
            self.marks[start:self.day_end[first - 1] + 1],
            self.times[begin - 1] if begin else self.first_ns, self.times[first - 1],
            config.annual_periods, config.zero_risk_free, begin > 0)
        return values["sharpe"]


def select(runs: Runs, candidates: list, config, training: int, validation: int):
    """Each round with every candidate's training Sharpe ratio and its choice,
    and for each trading day whom the account follows."""
    curves = [Curve(runs, identity) for identity in candidates]
    days = curves[0].days
    if any(curve.days != days for curve in curves):
        raise ValueError("the candidates did not settle the same trading days")
    followed = [None] * len(days)
    chosen = []
    for first, end in rounds(len(days), training, validation):
        begin = first - training
        scores = [curve.sharpe(begin, first, raw(config.deposit), config) for curve in curves]
        best = None
        for n, score in enumerate(scores):
            if score is not None and (best is None or score > scores[best]):
                best = n
        followed[first:end] = [best] * (end - first)
        chosen.append({"first_day": days[first], "training_first_day": days[begin],
                       "training_last_day": days[first - 1], "last_day": days[end - 1],
                       "sharpe": scores, "selected": best})
    return chosen, followed


def follow(engine, strategies: list, symbols: dict, contracts: list, followed: list):
    """One account over the days, every candidate reading every bar and the
    account taking the targets of whom it follows that day."""
    order = [symbols[unit] for unit in sorted(symbols)]
    contexts = [Context(order) for _ in strategies]
    units = {symbol: unit for unit, symbol in symbols.items()}
    standing = [{} for _ in strategies]  # unit -> lots each candidate wants

    def wants(whom):
        for unit in range(engine.units):
            if whom is None:
                engine.want(unit, 0)
            else:
                lots = standing[whom].get(unit)
                engine.want(unit, None if lots is None else lots * R)

    day = 0
    wants(followed[0])
    while not engine.finished:
        while not engine.day_done:
            bars = backtest.visible(engine.next(), symbols, contracts)
            for strategy, context, wanted in zip(strategies, contexts, standing):
                if bars:
                    for symbol, lots in step(strategy, context, bars).items():
                        wanted[units[symbol]] = lots
                    if context.mode == "weights":
                        raise ValueError("rolling validation follows strategies that target lots")
            wants(followed[day])
            engine.decide()
        engine.settle()
        day += 1
        # What the next round's choice wanted after the day before is what it trades.
        if not engine.finished and followed[day] != followed[day - 1]:
            wants(followed[day])
            engine.redecide()


def run(directory: Path, identity: str, sweep: str, config, training: int, validation: int):
    import asterion_engine

    runs = Runs(directory)
    members = runs.sweep(sweep)
    if len(members) < 2 or any(row["state"] != "succeeded" for row in members):
        raise ValueError("rolling validation needs two or more candidates that all succeeded")
    candidates = [row["id"] for row in members]
    chosen, followed = select(runs, candidates, config, training, validation)
    root = Path(config.repository)
    revision = repository.revision(repository.checked(root))
    warehouse = Warehouse(directory) if config.provider == "tushare" else \
        Warehouse(directory / "sources" / config.provider, config.provider)
    data = backtest.Data(warehouse, config.begin_day, config.end_day)
    runs.start(identity, Plan(
        repository=config.repository, strategy=config.strategy, revision=revision,
        params={"candidates": [row["params"] for row in members]}, provider=config.provider,
        dataset_version=data.version.identity, symbols=tuple(config.symbols),
        begin_day=config.begin_day, end_day=config.end_day, costs=backtest.costs_record(config),
        risk=backtest.risk_record(config),
        statistics={**backtest.statistics_record(config),
                    "rolling": {"sweep": sweep, "candidates": candidates, "training_days": training,
                                "validation_days": validation, "rounds": chosen}}))
    try:
        if data.version.identity != runs.record(candidates[0])["data"]["dataset_version"]:
            raise ValueError("the warehouse changed after the candidates ran")
        strategies = [repository.load(root, config.strategy, {**config.params, **row["params"]})
                      for row in members]
        contracts, series, first = backtest.prepare(data, config)
        engine = asterion_engine.Replay(
            raw(config.deposit), contracts, series,
            (raw(config.max_order_lots), raw(config.max_gross_lots), config.max_working_orders))
        symbols = {engine.unit(index): symbol for symbol, index in first.items()}
        follow(engine, strategies, symbols, contracts, followed)
        result = backtest.metrics(config, engine)
        # Out of sample: from the first validation day on.
        result.update({f"out_of_sample_{name}": value for name, value in
                       backtest.segment_metrics(config, engine, training).items()})
        runs.succeed(identity, backtest.tables(engine, contracts), result)
    except BaseException as error:
        runs.fail(identity, "failed", str(error) or type(error).__name__)
        raise


def main():
    from asterion.lab_pb2 import RollingRun

    backtest.watch_parent()
    request = RollingRun()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request.id, request.sweep, backtest.config_of(request.base),
            request.training_days, request.validation_days)
    except BaseException as error:
        print(error, file=sys.stderr)
        sys.exit(1)
