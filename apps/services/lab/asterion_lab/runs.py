"""Run records: what one backtest ran and what it produced.

    runs/<run>/run.json          what was run: strategy, parameters, data, engine, costs, risk
    runs/<run>/<table>.parquet   equity, orders, fills, positions and daily settlements
    runs.sqlite                  every run's state and metrics, for lists and rankings

run.json is written when the run starts and never changed. The tables are
written when it succeeds, before the index says so; a run that stops before
then keeps only what it started with. Amounts have the ledger's eight decimal
places, held exactly.
"""
from dataclasses import dataclass
import hashlib
import importlib.metadata
import io
import json
import math
from pathlib import Path
import re
import sqlite3
import time

import polars as pl

from asterion_lab.repository import Revision
from asterion_lab.version import VERSION
from asterion_lab.warehouse import write_durably

AMOUNT = pl.Decimal(19, 8)
TABLES = {
    "equity": {"time_ns": pl.Int64, "equity": AMOUNT},
    "orders": {"order": pl.String, "time_ns": pl.Int64, "symbol": pl.String,
               "contract": pl.String, "side": pl.String, "offset": pl.String,
               "quantity": AMOUNT, "price": AMOUNT, "state": pl.String},
    "fills": {"fill": pl.String, "order": pl.String, "time_ns": pl.Int64, "contract": pl.String,
              "side": pl.String, "offset": pl.String, "quantity": AMOUNT, "price": AMOUNT,
              "fee": AMOUNT},
    "positions": {"trading_day": pl.Date, "contract": pl.String, "long": AMOUNT,
                  "short": AMOUNT, "settlement_price": AMOUNT, "margin": AMOUNT},
    "settlements": {"trading_day": pl.Date, "time_ns": pl.Int64, "balance": AMOUNT,
                    "equity": AMOUNT, "realized": AMOUNT, "fees": AMOUNT, "margin": AMOUNT,
                    "available": AMOUNT},
}
IDENTITY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")


def requirements() -> str:
    """The digest of the packages this interpreter imports from: the bundled
    runtime's locked set."""
    installed = sorted(f"{item.metadata['Name']}=={item.version}"
                       for item in importlib.metadata.distributions())
    return hashlib.sha256("\n".join(installed).encode()).hexdigest()


@dataclass(frozen=True)
class Plan:
    """What a run is asked to do. Costs and risk are the parameters the engine
    applied, recorded as given."""
    repository: str
    strategy: str  # <module>.<class>
    revision: Revision
    params: dict
    provider: str
    dataset_version: str
    symbols: tuple
    begin_day: str
    end_day: str
    costs: dict
    risk: dict


class Runs:
    def __init__(self, directory: Path):
        self.root = directory / "runs"
        self.root.mkdir(parents=True, exist_ok=True)
        self.index = sqlite3.connect(directory / "runs.sqlite", check_same_thread=False,
                                     isolation_level=None)
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
                primary key (run, name));""")

    def start(self, identity: str, plan: Plan):
        if not IDENTITY.fullmatch(identity):
            raise ValueError(f"invalid run id: {identity}")
        directory = self.root / identity
        if directory.exists():
            raise ValueError(f"run {identity} already exists")
        started = time.time_ns()
        record = {"id": identity, "started_ns": started,
                  "strategy": {"repository": plan.repository, "id": plan.strategy,
                               "commit": plan.revision.commit, "changes": plan.revision.changes},
                  "params": plan.params,
                  "data": {"provider": plan.provider, "dataset_version": plan.dataset_version,
                           "symbols": list(plan.symbols), "begin_day": plan.begin_day,
                           "end_day": plan.end_day},
                  "engine": {"version": VERSION, "requirements": requirements()},
                  "costs": plan.costs, "risk": plan.risk}
        write_durably(directory / "run.json",
                      json.dumps(record, indent=2, ensure_ascii=False).encode() + b"\n")
        self.index.execute(
            "insert into runs (id, strategy, commit_id, changes, dataset_version, state, started_ns)"
            " values (?, ?, ?, ?, ?, 'running', ?)",
            (identity, plan.strategy, plan.revision.commit, plan.revision.changes,
             plan.dataset_version, started))

    def succeed(self, identity: str, tables: dict, metrics: dict):
        """Write the run's tables, each a polars frame of exactly its schema, then
        its metrics; a metric without a value (a ratio of nothing) is None."""
        if set(tables) != set(TABLES):
            raise ValueError(f"a run writes exactly the tables {', '.join(TABLES)}")
        for name, data in tables.items():
            if dict(data.schema) != TABLES[name]:
                raise ValueError(f"{name} does not have the run schema: {dict(data.schema)}")
        for name, value in metrics.items():
            if value is not None and not math.isfinite(value):
                raise ValueError(f"metric {name} is not finite")
        directory = self.root / identity
        for name, data in tables.items():
            stream = io.BytesIO()
            data.write_parquet(stream, compression="zstd", statistics=True)
            write_durably(directory / f"{name}.parquet", stream.getvalue())
        with self.index:
            self.index.execute("begin")
            self.index.executemany("insert into metrics (run, name, value) values (?, ?, ?)",
                                   [(identity, name, value) for name, value in metrics.items()])
            self.finish(identity, "succeeded", "")

    def fail(self, identity: str, state: str, error: str):
        if state not in ("failed", "interrupted"):
            raise ValueError(f"a run ends failed or interrupted, not {state}")
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

    def list(self) -> list:
        rows = self.index.execute(
            "select id, strategy, commit_id, changes, dataset_version, state, error, started_ns,"
            " finished_ns from runs order by started_ns desc").fetchall()
        metrics = {}
        for run, name, value in self.index.execute("select run, name, value from metrics"):
            metrics.setdefault(run, {})[name] = value
        names = ("id", "strategy", "commit", "changes", "dataset_version", "state", "error",
                 "started_ns", "finished_ns")
        return [{**dict(zip(names, row)), "metrics": metrics.get(row[0], {})} for row in rows]
