"""Run records: what one backtest ran and what it produced.

    runs/<run>/run.json          what was run: strategy, parameters, data, engine, costs, risk
    runs/<run>/<table>.parquet   equity, orders, fills, positions and daily settlements
    runs.sqlite                  every run's state and metrics, for lists and rankings,
                                 and the runs of each parameter sweep with their values

run.json is written when the run starts; on success its data gain what the
bars turned out to lack. The tables are written when it succeeds, before the
index says so; a run that stops before
then keeps only what it started with. Amounts have the ledger's eight decimal
places, held exactly.
"""
from dataclasses import dataclass, field
import io
import json
import math
from pathlib import Path
import re
import sqlite3
import time

import polars as pl

from asterion_lab.repository import Revision
from asterion_lab.version import VERSION, COMMIT
from asterion_lab.warehouse import write_durably

AMOUNT = pl.Decimal(19, 8)
TABLES = {
    # Every observation: "mark" after a bar, "settlement" at a day's end.
    "equity": {"time_ns": pl.Int64, "equity": AMOUNT, "event": pl.String},
    # time_ns is when it happened; trading_day the day it belongs to, a Monday for a Friday night.
    "orders": {"order": pl.String, "time_ns": pl.Int64, "trading_day": pl.Date, "symbol": pl.String,
               "contract": pl.String, "side": pl.String, "offset": pl.String,
               "quantity": AMOUNT, "price": AMOUNT, "state": pl.String},
    "fills": {"fill": pl.String, "order": pl.String, "time_ns": pl.Int64, "trading_day": pl.Date,
              "contract": pl.String,
              "side": pl.String, "offset": pl.String, "quantity": AMOUNT, "price": AMOUNT,
              "fee": AMOUNT},
    "positions": {"trading_day": pl.Date, "contract": pl.String, "long": AMOUNT,
                  "short": AMOUNT, "settlement_price": AMOUNT, "margin": AMOUNT},
    "settlements": {"trading_day": pl.Date, "time_ns": pl.Int64, "balance": AMOUNT,
                    "equity": AMOUNT, "realized": AMOUNT, "fees": AMOUNT, "margin": AMOUNT,
                    "available": AMOUNT},
}
IDENTITY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")


@dataclass(frozen=True)
class Plan:
    """What a run is asked to do. Costs and risk are the parameters the engine
    applied, recorded as given."""
    repository: str
    strategy: str  # <module>.<class>
    revision: Revision
    params: dict
    dataset_version: str
    symbols: tuple
    begin_day: str
    end_day: str
    costs: dict
    risk: dict
    statistics: dict
    # The bars a run reads: days, or minutes of a period from a source.
    bars: dict = field(default_factory=lambda: {"period": "day"})


class Runs:
    def __init__(self, directory: Path):
        self.root = directory / "runs"
        self.root.mkdir(parents=True, exist_ok=True)
        # Shared cached statements can lose rows during concurrent reads (CPython #118172).
        self.index = sqlite3.connect(directory / "runs.sqlite", check_same_thread=False,
                                     isolation_level=None, cached_statements=0)
        self.index.executescript("""
            pragma journal_mode = wal;
            pragma synchronous = full;
            create table if not exists runs (
                id text primary key, strategy text not null, commit_id text not null,
                changes text not null, dataset_version text not null, state text not null,
                error text not null default '', started_ns integer not null,
                finished_ns integer);
            create table if not exists metrics (
                run text not null references runs(id), name text not null, value real,
                primary key (run, name));
            create table if not exists sweeps (
                sweep text not null, position integer not null, run text not null unique,
                params text not null, primary key (sweep, position));""")

    def used(self, identity: str) -> bool:
        """An admitted run keeps its identity, even if its sweep was cancelled
        before the child started or the service restarted."""
        return (self.root / identity).exists() or self.index.execute(
            "select 1 from runs where id = ? union all "
            "select 1 from sweeps where run = ? limit 1", (identity, identity)).fetchone() is not None

    def start(self, identity: str, plan: Plan):
        if not IDENTITY.fullmatch(identity):
            raise ValueError(f"invalid run id: {identity}")
        directory = self.root / identity
        if directory.exists():
            raise ValueError(f"run {identity} already exists")
        self._insert(self._write_record(identity, plan), "running")

    def _write_record(self, identity: str, plan: Plan) -> dict:
        started = time.time_ns()
        record = {"id": identity, "started_ns": started,
                  "strategy": {"repository": plan.repository, "id": plan.strategy,
                               "commit": plan.revision.commit, "changes": plan.revision.changes},
                  "params": plan.params,
                  "data": {"dataset_version": plan.dataset_version,
                           "symbols": list(plan.symbols), "begin_day": plan.begin_day,
                           "end_day": plan.end_day, "bars": plan.bars},
                  "engine": {"version": VERSION, "commit": COMMIT},
                  "costs": plan.costs, "risk": plan.risk, "statistics": plan.statistics}
        write_durably(self.root / identity / "run.json",
                      json.dumps(record, indent=2, ensure_ascii=False).encode() + b"\n")
        return record

    def _insert(self, record: dict, state: str, error: str = ""):
        strategy = record["strategy"]
        self.index.execute(
            "insert into runs (id, strategy, commit_id, changes, dataset_version, state, error,"
            " started_ns, finished_ns) values (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (record["id"], strategy["id"], strategy["commit"], strategy["changes"],
             record["data"]["dataset_version"], state, error, record["started_ns"],
             None if state == "running" else time.time_ns()))

    def fail_start(self, identity: str, plan: Plan, error: str, state: str = "failed"):
        # The child may have written run.json before dying at its index insert.
        record = self.record(identity) if (self.root / identity / "run.json").exists() \
            else self._write_record(identity, plan)
        self._insert(record, state, error)

    def succeed(self, identity: str, tables: dict, metrics: dict, data: dict = {}):
        """Write the run's tables, each a polars frame of exactly its schema, then
        its metrics; a metric without a value (a ratio of nothing) is None.
        `data` joins the record's data."""
        if set(tables) != set(TABLES):
            raise ValueError(f"a run writes exactly the tables {', '.join(TABLES)}")
        for name, frame in tables.items():
            if dict(frame.schema) != TABLES[name]:
                raise ValueError(f"{name} does not have the run schema: {dict(frame.schema)}")
        for name, value in metrics.items():
            if value is not None and not math.isfinite(value):
                raise ValueError(f"metric {name} is not finite")
        directory = self.root / identity
        for name, frame in tables.items():
            stream = io.BytesIO()
            frame.write_parquet(stream, compression="zstd", statistics=True)
            write_durably(directory / f"{name}.parquet", stream.getvalue())
        if data:
            record = self.record(identity)
            record["data"].update(data)
            write_durably(directory / "run.json",
                          json.dumps(record, indent=2, ensure_ascii=False).encode() + b"\n")
        with self.index:
            self.index.execute("begin")
            self.index.executemany("insert into metrics (run, name, value) values (?, ?, ?)",
                                   [(identity, name, value) for name, value in metrics.items()])
            self.finish(identity, "succeeded", "")

    def end(self, identity: str, state: str, error: str):
        """How a run ended without results: failed, interrupted from outside,
        or, a live instance, stopped by its owner."""
        if state not in ("failed", "interrupted", "stopped"):
            raise ValueError(f"a run ends failed, interrupted or stopped, not {state}")
        self.finish(identity, state, error)

    def finish(self, identity: str, state: str, error: str):
        changed = self.index.execute(
            "update runs set state = ?, error = ?, finished_ns = ? where id = ? and state = 'running'",
            (state, error, time.time_ns(), identity)).rowcount
        if changed != 1:
            raise ValueError(f"run {identity} is not running")

    def record(self, identity: str) -> dict:
        return json.loads((self.root / identity / "run.json").read_text())

    def table(self, identity: str, name: str) -> pl.DataFrame:
        if name not in TABLES:
            raise ValueError(f"no run table {name}")
        return pl.read_parquet(self.root / identity / f"{name}.parquet")

    def interrupt(self):
        """Marks the runs a stopped service left running as interrupted; they
        are not run again."""
        self.index.execute(
            "update runs set state = 'interrupted', error = 'the lab service stopped',"
            " finished_ns = ? where state = 'running'", (time.time_ns(),))

    def plan_sweep(self, sweep: str, runs: list):
        """The runs of a sweep, each (run id, parameter values), before any starts."""
        if self.index.execute("select 1 from sweeps where sweep = ?", (sweep,)).fetchone():
            raise ValueError(f"sweep {sweep} already exists")
        with self.index:
            self.index.execute("begin")
            self.index.executemany(
                "insert into sweeps (sweep, position, run, params) values (?, ?, ?, ?)",
                [(sweep, n, run, json.dumps(params, sort_keys=True))
                 for n, (run, params) in enumerate(runs)])

    def sweep(self, sweep: str) -> list:
        """A sweep's runs in order, each with its parameter values; a run not
        started yet, or never started, has no state."""
        rows = self.index.execute(
            "select run, params from sweeps where sweep = ? order by position", (sweep,)).fetchall()
        if not rows:
            raise ValueError(f"no sweep {sweep}")
        listed = {row["id"]: row for row in self.list()}
        return [{**listed.get(run, {"id": run, "state": "", "metrics": {}}),
                 "params": json.loads(params)} for run, params in rows]

    def list(self) -> list:
        rows = self.index.execute(
            "select id, strategy, commit_id, changes, dataset_version, state, error, started_ns,"
            " finished_ns from runs order by started_ns desc").fetchall()
        metrics = {}
        for run, name, value in self.index.execute("select run, name, value from metrics"):
            metrics.setdefault(run, {})[name] = value
        sweeps = dict(self.index.execute("select run, sweep from sweeps"))
        names = ("id", "strategy", "commit", "changes", "dataset_version", "state", "error",
                 "started_ns", "finished_ns")
        return [{**dict(zip(names, row)), "metrics": metrics.get(row[0], {}),
                 "sweep": sweeps.get(row[0], "")} for row in rows]
