"""Typed research views of immutable cached Parquet partitions.

Market owns publication. This module contains no acquisition or warehouse writer.
"""
from dataclasses import dataclass
import json
import os
from pathlib import Path

import duckdb
import polars as pl

NUMBER = pl.Decimal(28, 6)
PRICE_TICK = pl.Decimal(28, 8)
SCHEMAS = {
    "daily": {"exchange": pl.String, "product": pl.String, "contract": pl.String, "day": pl.Date,
              **{name: NUMBER for name in (
                  "open", "high", "low", "close", "settlement", "previous_close",
                  "previous_settlement", "volume", "amount", "open_interest",
                  "open_interest_change", "delivery_settlement")}},
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


@dataclass
class Version:
    identity: str
    manifest: dict

    def keys(self, table: str, exchange: str = "", contract: str = "", begin: str = "", end: str = ""):
        """The partitions of `table`, of one exchange when named. A minute
        partition is one contract's trading day (minutes/<exchange>/<day>/<product>/<month>),
        so minutes also narrow to `contract` and to days in [begin, end]."""
        for key in sorted(self.manifest["partitions"]):
            parts = key.split("/")
            if parts[0] != table or (exchange and parts[1] != exchange):
                continue
            if table == "minutes" and ((contract and "/".join([parts[1], *parts[3:]]) != contract)
                                       or (begin and parts[2] < begin) or (end and parts[2] > end)):
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
        days = set()
        for key, entry in self.manifest["partitions"].items():
            if key.startswith(f"daily/{exchange}/"):
                days.update(entry["days"])
        return days

    def has(self, key: str) -> bool:
        return key in self.manifest["partitions"]


class ParquetTables:
    """Queries against immutable files selected by the read-only cache."""

    def query(self, version: Version, sql: str, tables: tuple, parameters=(), **scope):
        """Run `sql` with each of `tables` bound to the version's files, of the
        exchange, contract and days `scope` names (see Version.keys)."""
        connection = duckdb.connect()
        try:
            for table in tables:
                files = version.files(self.root, table, **scope)
                if files:
                    # Views take no parameters; the paths are the warehouse's own.
                    listed = ", ".join("'" + path.replace("'", "''") + "'" for path in files)
                    connection.execute(
                        f"create view {table} as select * from read_parquet([{listed}])")
                else:
                    names = ", ".join(f"null::{SQL_TYPES[kind]} as {name}"
                                      for name, kind in SCHEMAS[table].items())
                    connection.execute(f"create view {table} as select {names} where false")
            return connection.execute(sql, list(parameters)).fetchall()
        finally:
            connection.close()

    def read(self, version: Version, key: str) -> pl.DataFrame:
        return pl.read_parquet(self.root / version.manifest["partitions"][key]["file"])
