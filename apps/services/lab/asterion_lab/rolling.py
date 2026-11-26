"""Rolling validation of a sweep, (docs/lab/README.md, 参数扫描).

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
from dataclasses import replace
from pathlib import Path

from asterion_lab import backtest, data as lab_data, diagnostics, repository, statistics as lab_stats
from asterion_lab.data import R, raw
from asterion_lab.runs import Runs
from asterion_strategy import Context, quantize_weight, step

MIN_TRAINING = 20
MAX_ROUNDS = 100
OBJECTIVES = frozenset({"sharpe", "dsr", "calmar", "total_return"})


def normalize_objective(objective: str) -> str:
    value = objective or "sharpe"
    if value not in OBJECTIVES:
        raise ValueError(f"unknown walk-forward objective: {objective!r}")
    return value


def rounds(days: int, training: int, validation: int, embargo: int = 0) -> list:
    """Each round's first validation day and the day after its last, by index.

    Reference:
        López de Prado, M. (2018). Advances in Financial Machine Learning.
        Wiley, Ch. 7 (Purging and Embargoing).
    """
    if training < MIN_TRAINING or validation < 1 or embargo < 0 or training + embargo >= days:
        raise ValueError(f"rolling validation needs at least {MIN_TRAINING} training days, "
                         "nonnegative embargo days, validation days after them and one validation day a round")
    first_valid = training + embargo
    result = [(first, min(days, first + validation)) for first in range(first_valid, days, validation)]
    if len(result) > MAX_ROUNDS:
        raise ValueError(f"rolling validation has at most {MAX_ROUNDS} rounds, not {len(result)}")
    return result


class Curve:
    """A candidate's equity record by trading day."""

    def __init__(self, runs: Runs, identity: str):
        equity = runs.table(identity, "equity")
        self.marks = [raw(value) for value in equity["equity"]]
        self.first_ns = equity["time_ns"][0]
        self.day_end = [n for n, event in enumerate(equity["event"]) if event == "settlement"]
        settled = runs.table(identity, "settlements")
        self.days = [day.isoformat() for day in settled["trading_day"]]
        self.equity = [raw(value) for value in settled["equity"]]
        self.times = list(settled["time_ns"])

    def sharpe(self, begin: int, first: int, deposit: int, config):
        """The Sharpe ratio of training days [begin, first), from the equity of
        the day before them."""
        return self.score(begin, first, deposit, config, "sharpe", 1)

    def score(self, begin: int, first: int, deposit: int, config, objective: str, trials: int):
        """Evaluate the walk-forward selection objective over training slice [begin, first)."""
        import asterion_engine

        base = self.equity[begin - 1] if begin else deposit
        if base <= 0:
            return None
        if objective == "dsr":
            rets = []
            prev = float(base)
            for eq in self.equity[begin:first]:
                cur = float(eq)
                rets.append((cur / prev - 1.0) if prev > 0 else 0.0)
                prev = cur
            res = lab_stats.deflated_sharpe_ratio(
                rets,
                trials=max(1, trials),
                annual_periods=float(config.annual_periods),
            )
            return res["dsr"] if res is not None else None
        start = self.day_end[begin - 1] + 1 if begin else 0
        values = asterion_engine.performance(
            base, list(zip(self.days[begin:first], self.equity[begin:first])),
            self.marks[start:self.day_end[first - 1] + 1],
            self.times[begin - 1] if begin else self.first_ns, self.times[first - 1],
            config.annual_periods, config.zero_risk_free, True)
        return values.get(objective)


def select(
    runs: Runs,
    candidates: list,
    config,
    training: int,
    validation: int,
    embargo: int = 0,
    objective: str = "sharpe",
):
    """Each round with every candidate's training objective score and its choice,
    and for each trading day whom the account follows."""
    if not config.annual_periods or not config.zero_risk_free:
        raise ValueError("rolling validation requires periods_per_year and zero_risk_free_rate")
    obj = normalize_objective(objective)
    curves = [Curve(runs, identity) for identity in candidates]
    days = curves[0].days
    if any(curve.days != days for curve in curves):
        raise ValueError("the candidates did not settle the same trading days")
    followed = [None] * len(days)
    chosen = []
    trials = len(candidates)
    for first, end in rounds(len(days), training, validation, embargo):
        train_end = first - embargo
        begin = train_end - training
        scores = [
            curve.score(begin, train_end, raw(config.deposit), config, obj, trials)
            for curve in curves
        ]
        best = None
        for n, score in enumerate(scores):
            if score is not None and (best is None or score > scores[best]):
                best = n
        followed[first:end] = [best] * (end - first)
        chosen.append({"first_day": days[first], "training_first_day": days[begin],
                       "training_last_day": days[train_end - 1], "last_day": days[end - 1],
                       "sharpe": scores, "selected": best})
    return chosen, followed


def follow(engine, strategies: list, symbols: dict, contracts: list, followed: list, config=None):
    """One account over the days, every candidate reading every bar and the
    account taking the targets of whom it follows that day."""
    order = [symbols[unit] for unit in sorted(symbols)]
    contexts = [
        Context(order, backtest.rolls(contracts), max_history=getattr(strategy, "history_limit", None))
        for strategy in strategies
    ]
    units = {symbol: unit for unit, symbol in symbols.items()}
    standing = [{} for _ in strategies]  # unit -> lots (target mode)
    standing_weights = [{} for _ in strategies]  # unit -> raw weight (weights mode)
    paused = set()  # candidates not ready at their last bar

    def size_weights(whom):
        if config is None or config.max_gross_weight <= 0 or config.max_net_weight < 0:
            raise ValueError("rolling validation of weight strategies requires weight budgets")
        if not standing_weights[whom]:
            for unit in range(engine.units):
                engine.want(unit, 0)
            return
        sized = engine.weights(
            standing_weights[whom],
            raw(config.max_gross_weight),
            raw(config.max_net_weight),
            raw(config.max_margin_ratio),
            raw(config.min_rebalance_weight),
        )
        for unit in range(engine.units):
            engine.want(unit, sized.get(unit, 0))

    def wants(whom, resize=False):
        if whom is None:
            for unit in range(engine.units):
                engine.want(unit, 0)
            return
        if whom in paused:
            for unit in range(engine.units):
                engine.want(unit, None)
            return
        if contexts[whom].mode == "weights":
            if resize:
                size_weights(whom)
            return
        for unit in range(engine.units):
            lots = standing[whom].get(unit)
            engine.want(unit, None if lots is None else lots * R)

    day = 0
    wants(followed[0], resize=True)
    while not engine.finished:
        while not engine.day_done:
            bars = backtest.visible(engine.next(), symbols, contracts)
            reweighted_active = False
            for n, (strategy, context) in enumerate(zip(strategies, contexts)):
                if not bars:
                    continue
                wanted = step(strategy, context, bars)
                if not context.ready:
                    standing[n].clear()
                    standing_weights[n].clear()
                    paused.add(n)
                    continue
                paused.discard(n)
                if context.mode == "weights":
                    if wanted:
                        standing_weights[n] = {
                            units[symbol]: raw(quantize_weight(weight))
                            for symbol, weight in wanted.items()
                        }
                        if n == followed[day]:
                            reweighted_active = True
                else:
                    for symbol, lots in wanted.items():
                        standing[n][units[symbol]] = lots
            wants(followed[day], resize=reweighted_active)
            if followed[day] not in paused:
                engine.decide()
        engine.settle()
        day += 1
        # What the next round's choice wanted after the day before is what it trades.
        if not engine.finished and followed[day] != followed[day - 1]:
            wants(followed[day], resize=True)
            if followed[day] not in paused:
                engine.redecide()


def plan(
    config,
    version,
    sweep,
    members,
    training,
    validation,
    rounds,
    embargo: int = 0,
    objective: str = "sharpe",
):
    base = backtest.plan(config, version)
    return replace(base, params={"candidates": [row["params"] for row in members]},
                   statistics={**base.statistics,
                               "rolling": {"sweep": sweep, "candidates": [row["id"] for row in members],
                                           "training_days": training, "validation_days": validation,
                                           "embargo_days": embargo, "objective": normalize_objective(objective),
                                           "rounds": rounds}})


def run(
    directory: Path,
    identity: str,
    sweep: str,
    config,
    training: int,
    validation: int,
    started=lambda: None,
    embargo: int = 0,
    objective: str = "sharpe",
):
    import asterion_engine

    runs = Runs(directory)
    members = runs.sweep(sweep)
    if len(members) < 2 or any(row["state"] != "succeeded" for row in members):
        raise ValueError("rolling validation needs two or more candidates that all succeeded")
    candidates = [row["id"] for row in members]
    chosen, followed = select(runs, candidates, config, training, validation, embargo, objective)
    data = lab_data.open_data(directory, config)
    runs.start(
        identity,
        plan(config, data.version.identity, sweep, members, training, validation, chosen, embargo, objective),
    )
    started()
    try:
        for candidate in candidates:
            recorded = runs.record(candidate)["data"]
            if (recorded["dataset_version"] != data.version.identity
                    or recorded["bars"] != backtest.bars_record(config)):
                raise ValueError(f"candidate {candidate} used different dataset versions or bar inputs")
        strategies = [repository.load(Path(config.source), config.strategy,
                                      {**config.params, **row["params"]}) for row in members]
        contracts, series, first = lab_data.prepare(data, config)
        engine = asterion_engine.Replay(
            raw(config.deposit), contracts, series,
            (raw(config.max_order_lots), raw(config.max_gross_lots), config.max_working_orders,
             raw(config.max_contract_lots)))
        symbols = {engine.unit(index): symbol for symbol, index in first.items()}
        follow(engine, strategies, symbols, contracts, followed, config)
        backtest.held_without_prices(engine, contracts)
        result = backtest.metrics(config, engine)
        # Out of sample: from the first validation day on (after training + embargo).
        result.update({f"out_of_sample_{name}": value for name, value in
                       backtest.segment_metrics(config, engine, training + embargo).items()})
        run_tables = backtest.tables(engine, contracts)
        run_diag = diagnostics.compute(
            config,
            contracts,
            run_tables,
            series=series,
            trials=runs.strategy_trials(config.strategy, config.holdout_day),
            uses_pit_fundamentals=backtest.uses_pit_fundamentals(config.source),
        )
        runs.succeed(
            identity,
            run_tables,
            result,
            backtest.replay_record(
                engine,
                contracts,
                config.minutes,
                run_diagnostics=run_diag,
                lineage=backtest.strategy_lineage(strategies[0]) if strategies else None,
            ),
        )
    except BaseException as error:
        runs.end(identity, "failed", str(error) or type(error).__name__)
        raise


def main():
    from asterion.lab_pb2 import RollingRun

    request = RollingRun()
    request.ParseFromString(sys.stdin.buffer.read())
    try:
        run(Path(sys.argv[1]), request.id, request.sweep_id, backtest.config_of(request.base),
            request.walk_forward.training_days, request.walk_forward.validation_days,
            lambda: print("started", flush=True),
            embargo=request.walk_forward.embargo_days,
            objective=request.walk_forward.objective)
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.exit(1)
