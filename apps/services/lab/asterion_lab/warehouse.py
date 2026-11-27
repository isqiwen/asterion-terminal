"""Typed research views of immutable cached Parquet partitions.

Warehouse owns publication. This module contains no acquisition or warehouse writer.
"""
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import threading

import duckdb
import polars as pl

NUMBER = pl.Decimal(28, 6)
PRICE_TICK = pl.Decimal(28, 8)
SCHEMAS = {
    "daily": {"exchange": pl.String, "product": pl.String, "contract": pl.String, "day": pl.Date,
              **{name: NUMBER for name in (
                  "open", "high", "low", "close", "settlement", "previous_close",
                  "previous_settlement", "volume", "amount", "open_interest",
                  "open_interest_change", "delivery_settlement",
                  "lower_limit", "upper_limit")}},
    "calendar": {"exchange": pl.String, "day": pl.Date, "open": pl.Boolean,
                 "previous_trading_day": pl.Date},
    "contracts": {"exchange": pl.String, "contract": pl.String, "product": pl.String,
                  "delivery_month": pl.String, "source_instrument": pl.String, "name": pl.String,
                  "list_date": pl.Date, "delist_date": pl.Date, "last_delivery_date": pl.Date,
                  "multiplier": NUMBER, "per_unit": NUMBER, "trade_unit": pl.String,
                  "quote_unit": pl.String, "tick_description": pl.String,
                  "delivery_description": pl.String, "trading_hours": pl.String},
    "price_ticks": {"exchange": pl.String, "product": pl.String, "contract": pl.String,
                    "effective_from": pl.Date, "effective_until": pl.Date,
                    "price_tick": PRICE_TICK, "source_url": pl.String},
}

SCHEMAS["minutes"] = {"exchange": pl.String, "product": pl.String, "contract": pl.String,
    "day": pl.Date, "start_ms": pl.Int64, "partial": pl.Boolean,
    **{name: NUMBER for name in ("open", "high", "low", "close", "volume", "average_price", "open_interest")}}
SCHEMAS["settlement"] = {"exchange": pl.String, "product": pl.String, "contract": pl.String,
    "day": pl.Date, **{name: NUMBER for name in ("long_margin_rate", "short_margin_rate",
        "fee_rate", "fee_per_lot", "close_today_fee_rate", "close_today_fee_per_lot")}}
SCHEMAS["warehouse_receipts"] = {"exchange": pl.String, "product": pl.String, "day": pl.Date,
    **{name: pl.String for name in ("warehouse", "warehouse_id", "warehouse_type", "area", "crop_year", "grade",
        "brand", "origin", "unit", "converted")},
    **{name: NUMBER for name in ("previous_volume", "volume", "volume_change", "forecast_volume", "premium")},
    "published_ms": pl.Int64, "observed_ms": pl.Int64}
SCHEMAS["member_positions"] = {"exchange": pl.String, "product": pl.String, "contract": pl.String,
    "day": pl.Date, "member": pl.String,
    **{name: NUMBER for name in ("volume", "volume_change", "long_position", "long_change", "short_position", "short_change")},
    "published_ms": pl.Int64, "observed_ms": pl.Int64}
SCHEMAS["inventory"] = {"exchange": pl.String, "product": pl.String, "day": pl.Date,
    **{name: pl.String for name in ("area", "warehouse", "warehouse_type", "grade", "unit")},
    **{name: NUMBER for name in ("previous_total", "previous_receipts", "total", "receipts",
        "total_change", "receipts_change", "previous_capacity", "capacity", "capacity_change")},
    "published_ms": pl.Int64, "observed_ms": pl.Int64}

SQL_TYPES = {pl.Int64: "bigint", pl.String: "varchar", pl.Date: "date", pl.Boolean: "boolean",
             NUMBER: "decimal(28, 6)", PRICE_TICK: "decimal(28, 8)"}


def write_durably(path: Path, data: bytes):
    """Write, flush to disk, then publish by rename, so a reader sees all or nothing."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


DATED_TABLES = frozenset(
    ("daily", "minutes", "settlement", "warehouse_receipts", "member_positions", "inventory")
)


@dataclass
class Version:
    identity: str
    manifest: dict
    _by_table: dict | None = field(default=None, init=False, repr=False, compare=False)
    _minute_by_contract: dict | None = field(default=None, init=False, repr=False, compare=False)
    _indexed_len: int = field(default=-1, init=False, repr=False, compare=False)

    def _ensure_index(self):
        partitions = self.manifest["partitions"]
        if self._by_table is not None and self._indexed_len == len(partitions):
            return
        by_table = {}
        minute_by_contract = {}
        for key in sorted(partitions):
            parts = key.split("/")
            table, exchange = parts[0], parts[1]
            date = parts[2] if table in DATED_TABLES else ""
            by_table.setdefault(table, {}).setdefault(exchange, []).append((key, date))
            if table == "minutes":
                name = "/".join([exchange, *parts[3:]])
                minute_by_contract.setdefault(name, []).append((key, exchange, date))
        self._by_table = by_table
        self._minute_by_contract = minute_by_contract
        self._indexed_len = len(partitions)

    def keys(self, table: str, exchange: str = "", contract: str = "", begin: str = "", end: str = ""):
        """The partitions of `table`, of one exchange when named. A minute
        partition is one contract's trading day (minutes/<exchange>/<day>/<product>/<month>),
        so minutes also narrow to `contract`. Dated partitions intersect [begin, end]."""
        self._ensure_index()
        if table == "minutes" and contract:
            for key, part_exchange, date in self._minute_by_contract.get(contract, ()):
                if exchange and part_exchange != exchange:
                    continue
                if (begin and date < begin[:len(date)]) or (end and date > end[:len(date)]):
                    continue
                yield key
            return
        by_exchange = self._by_table.get(table)
        if not by_exchange:
            return
        groups = (by_exchange.get(exchange, ()),) if exchange else by_exchange.values()
        dated = table in DATED_TABLES
        for group in groups:
            for key, date in group:
                if dated and ((begin and date < begin[:len(date)]) or (end and date > end[:len(date)])):
                    continue
                yield key

    def files(self, root: Path, table: str, **scope) -> list:
        return [str(root / self.manifest["partitions"][key]["file"]) for key in self.keys(table, **scope)]

    def minute_coverage(self, contract: str = "") -> dict:
        """Each contract's stored minute days, (first, last, count), from the
        manifest alone: nothing is downloaded."""
        days = {}
        for key in self.keys("minutes", contract=contract):
            parts = key.split("/")
            days.setdefault("/".join([parts[1], *parts[3:]]), []).append(parts[2])
        return {name: (min(found), max(found), len(set(found))) for name, found in days.items()}

    def days(self, exchange: str) -> set:
        self._ensure_index()
        partitions = self.manifest["partitions"]
        days = set()
        for key, _ in self._by_table.get("daily", {}).get(exchange, ()):
            days.update(partitions[key]["days"])
        return days

    def has(self, key: str) -> bool:
        return key in self.manifest["partitions"]


class ParquetTables:
    """Queries against immutable files selected by the read-only cache."""

    def _state(self):
        local = self.__dict__.get("_duckdb_local")
        if local is None:
            local = threading.local()
            self.__dict__["_duckdb_local"] = local
        if not hasattr(local, "depth"):
            local.depth = 0
            local.connection = None
        return local

    def __enter__(self):
        self._state().depth += 1
        return self

    def __exit__(self, *ignored):
        state = self._state()
        state.depth = max(0, state.depth - 1)
        if not state.depth and state.connection is not None:
            try:
                state.connection.close()
            finally:
                state.connection = None

    def query(self, version: Version, sql: str, tables: tuple, parameters=(), **scope):
        """Run `sql` with each of `tables` bound to the version's files, of the
        exchange, contract and days `scope` names (see Version.keys)."""
        state = self._state()
        shared = state.depth > 0
        if shared:
            if state.connection is None:
                state.connection = duckdb.connect()
            connection = state.connection
        else:
            connection = duckdb.connect()
        try:
            for table in tables:
                files = version.files(self.root, table, **scope)
                if files:
                    # Views take no parameters; the paths are the warehouse's own.
                    listed = ", ".join("'" + path.replace("'", "''") + "'" for path in files)
                    connection.execute(
                        f"create or replace temp view {table} as select * from read_parquet([{listed}])")
                else:
                    names = ", ".join(f"null::{SQL_TYPES[kind]} as {name}"
                                      for name, kind in SCHEMAS[table].items())
                    connection.execute(
                        f"create or replace temp view {table} as select {names} where false")
            return connection.execute(sql, list(parameters)).fetchall()
        finally:
            if shared:
                for table in tables:
                    connection.execute(f"drop view if exists {table}")
            else:
                connection.close()

    def scan(self, version: Version, table: str, **scope) -> pl.LazyFrame:
        """Lazy Polars scan over the version's partitions selected by `scope`."""
        files = version.files(self.root, table, **scope)
        if not files:
            return pl.DataFrame(schema=SCHEMAS[table]).lazy()
        return pl.scan_parquet(files)

    def read(self, version: Version, key: str) -> pl.DataFrame:
        return pl.read_parquet(self.root / version.manifest["partitions"][key]["file"])
