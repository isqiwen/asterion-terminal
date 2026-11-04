"""Test-only Parquet writer and prefilled remote cache for replay unit tests."""
import datetime
from decimal import Decimal
import hashlib
import io
import json
import re
from pathlib import Path
import shutil
import time
import polars as pl
from asterion_lab.warehouse import ParquetTables, Version, SCHEMAS, NUMBER, SCALE, canonical, write_durably

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


class Dataset(ParquetTables):
    """Read the test writer's local publication, not a production fallback.
    `directory` is Market's data directory; research reads provider "research"."""
    def __init__(self, directory, provider="research"):
        self.directory, self.provider = Path(directory), provider
        source = self.directory if provider == "tushare" else self.directory / "sources" / provider
        self.root = source / "warehouse"

    def current(self):
        pointer = self.root / "current"
        if not pointer.exists():
            return Version("", {"partitions": {}})
        return self.version(pointer.read_text().strip())

    def version(self, identity):
        if not re.fullmatch(r"[0-9a-f]{64}", identity):
            raise ValueError("invalid test version")
        manifest = json.loads((self.root / "versions" / f"{identity}.json").read_text())
        partitions = {}
        for shard in manifest.pop("shards").values():
            partitions.update(json.loads((self.root / "shards" / f"{shard}.json").read_text()))
        return Version(identity, {**manifest, "partitions": partitions})


class Warehouse(Dataset):
    """The acquisition owner's writable store; research only opens Dataset."""

    def __init__(self, directory: Path, provider="research"):
        super().__init__(directory, provider)
        (self.root / "versions").mkdir(parents=True, exist_ok=True)
        (self.root / "files").mkdir(exist_ok=True)

    def writer(self) -> "Writer":
        return Writer(self, self.current())


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
        # Market's layout: partitions in shards of one table, exchange and month.
        shards = {}
        for key, entry in self.partitions.items():
            table, exchange, *rest = key.split("/")
            name = "/".join([table, exchange] + ([rest[0][:7]] if rest else []))
            shards.setdefault(name, {})[key] = entry
        names = {}
        for name, partitions in shards.items():
            content = canonical(partitions)
            names[name] = hashlib.sha256(content).hexdigest()
            write_durably(self.warehouse.root / "shards" / f"{names[name]}.json", content)
        manifest = {"format": 2, "provider": self.warehouse.provider, "parent": self.base.identity or None,
                    "created_ns": time.time_ns(), "shards": names}
        content = canonical(manifest)
        identity = hashlib.sha256(content).hexdigest()
        write_durably(self.warehouse.root / "versions" / f"{identity}.json", content)
        write_durably(self.warehouse.root / "current", identity.encode())
        del manifest["shards"]
        self.base = Version(identity, {**manifest, "partitions": self.partitions})
        self.partitions = dict(self.partitions)
        cache_version(self.warehouse.directory, self.warehouse.root, self.warehouse.provider, self.base)
        return self.base


def cache_version(lab, store, provider, version):
    """The version as Lab in `lab` would have cached it from Market."""
    target = lab / "market-cache" / provider
    parts = {}
    for key, part in version.manifest["partitions"].items():
        relative = f"files/{part['sha256']}.parquet"
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            shutil.copyfile(store / part["file"], path)
        parts[key] = {**part, "file": relative}
    manifest = {**version.manifest, "partitions": parts,
                "origin": {"host": "127.0.0.1", "port": 1, "fingerprint": "0" * 64,
                           "certificate_file": "/missing-test-certificate", "private_key_file": "/missing-test-key"}}
    write_durably(target / "versions" / f"{version.identity}.json", canonical(manifest))
