"""The futures data warehouse: partitioned Parquet files and dataset versions.

    warehouse/files/<table>/<exchange>/<partition>-<digest>.parquet
    warehouse/versions/<version>.json
    warehouse/current

A partition file is written once and named by its content. A version is the
list of every partition file with its digest; its identity is the digest of
that list. `current` names the newest version. Only the acquisition writer
changes the warehouse, and only by committing a new version: what a version
lists is never rewritten or deleted.
"""
from dataclasses import dataclass
import datetime
from decimal import Decimal
import hashlib
import io
import json
import os
from pathlib import Path
import time

import duckdb
import polars as pl

SCALE = Decimal("0.000001")
NUMBER = pl.Decimal(28, 6)
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
}

SQL_TYPES = {pl.String: "varchar", pl.Date: "date", pl.Boolean: "boolean",
             NUMBER: "decimal(28, 6)"}


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


def exact(value, column: str):
    """A decimal the warehouse holds exactly, refused rather than rounded."""
    if value is None:
        return None
    value = Decimal(value)
    if value != value.quantize(SCALE) or abs(value) >= Decimal(10) ** 22:
        raise ValueError(f"{column} {value} does not fit six decimal places")
    return value


def frame(table: str, rows: list) -> pl.DataFrame:
    schema = SCHEMAS[table]
    columns = {}
    for name, kind in schema.items():
        values = [row[name] for row in rows]
        if kind == NUMBER:
            values = [exact(value, name) for value in values]
        elif kind == pl.Date:
            values = [None if value is None else datetime.date.fromisoformat(value)
                      for value in values]
        columns[name] = pl.Series(name, values, dtype=kind)
    return pl.DataFrame(columns)


@dataclass
class Version:
    identity: str
    manifest: dict

    def files(self, root: Path, table: str, exchange: str = "") -> list:
        return [str(root / entry["file"]) for key, entry in sorted(self.manifest["partitions"].items())
                if key.split("/")[0] == table and (not exchange or key.split("/")[1] == exchange)]

    def days(self, exchange: str) -> set:
        days = set()
        for key, entry in self.manifest["partitions"].items():
            if key.startswith(f"daily/{exchange}/"):
                days.update(entry["days"])
        return days

    def has(self, key: str) -> bool:
        return key in self.manifest["partitions"]


EMPTY = Version("", {"format": 1, "parent": None, "created_ns": 0, "partitions": {}})


class Warehouse:
    def __init__(self, directory: Path):
        self.root = directory / "warehouse"
        (self.root / "versions").mkdir(parents=True, exist_ok=True)
        (self.root / "files").mkdir(exist_ok=True)

    def current(self) -> Version:
        pointer = self.root / "current"
        if not pointer.exists():
            return EMPTY
        identity = pointer.read_text().strip()
        path = self.root / "versions" / f"{identity}.json"
        try:
            manifest = json.loads(path.read_text())
        except (OSError, ValueError) as error:
            raise ValueError(f"unreadable dataset version: {path}: {error}")
        return Version(identity, manifest)

    def writer(self) -> "Writer":
        return Writer(self, self.current())

    def query(self, version: Version, sql: str, tables: tuple, parameters=(), exchange=""):
        """Run `sql` with each of `tables` bound to the version's files, of one
        exchange when it is named."""
        connection = duckdb.connect()
        try:
            for table in tables:
                files = version.files(self.root, table, exchange)
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


class Writer:
    """Partitions for the next version; nothing is visible before commit."""

    def __init__(self, warehouse: Warehouse, base: Version):
        self.warehouse = warehouse
        self.base = base
        self.partitions = dict(base.manifest["partitions"])

    def put(self, key: str, table: str, data: pl.DataFrame, **extra):
        stream = io.BytesIO()
        data.write_parquet(stream, compression="zstd", statistics=True)
        content = stream.getvalue()
        digest = hashlib.sha256(content).hexdigest()
        parts = key.split("/")
        relative = Path("files", *parts[:-1], f"{parts[-1]}-{digest[:16]}.parquet")
        path = self.warehouse.root / relative
        if not path.exists():
            write_durably(path, content)
        self.partitions[key] = {"file": relative.as_posix(), "sha256": digest,
                                "rows": data.height, **extra}

    def put_rows(self, key: str, table: str, rows: list, **extra):
        self.put(key, table, frame(table, rows), **extra)

    def add_days(self, exchange: str, month: str, rows: list, days: list):
        """Merge days of daily bars into the month's partition."""
        key = f"daily/{exchange}/{month}"
        added = frame("daily", rows)
        known = set(days)
        if key in self.partitions:
            earlier = pl.read_parquet(self.warehouse.root / self.partitions[key]["file"])
            earlier = earlier.filter(~pl.col("day").cast(pl.String).is_in(list(known)))
            added = pl.concat([earlier, added])
            known |= set(self.partitions[key]["days"])
        added = added.sort("day", "contract")
        self.put(key, "daily", added, days=sorted(known))

    def commit(self) -> Version:
        if self.partitions == self.base.manifest["partitions"]:
            return self.base
        manifest = {"format": 1, "parent": self.base.identity or None,
                    "created_ns": time.time_ns(), "partitions": self.partitions}
        content = canonical(manifest)
        identity = hashlib.sha256(content).hexdigest()
        write_durably(self.warehouse.root / "versions" / f"{identity}.json", content)
        write_durably(self.warehouse.root / "current", identity.encode())
        self.base = Version(identity, manifest)
        self.partitions = dict(self.partitions)
        return self.base
