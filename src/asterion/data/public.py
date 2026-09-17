"""Data interface v1. CSV imports are immutable; invalid input fails atomically."""

import csv
import hashlib
import io
from datetime import date, datetime
from decimal import Decimal
from typing import Annotated

import pyarrow as pa
import pyarrow.parquet as pq
from pydantic import AwareDatetime, BaseModel, Field, model_validator

Price = Annotated[Decimal, Field(gt=0, max_digits=20, decimal_places=8)]


class Bar(BaseModel):
    contract: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}$")
    event_time: AwareDatetime
    available_at: AwareDatetime
    trading_day: date
    open: Price
    high: Price
    low: Price
    close: Price
    volume: int = Field(ge=0)

    @model_validator(mode="after")
    def check(self):
        if self.event_time.microsecond:
            raise ValueError("CSV bar v1 requires whole-second event_time")
        if self.low > min(self.open, self.close) or self.high < max(self.open, self.close):
            raise ValueError("OHLC price bounds are inconsistent")
        if self.available_at < self.event_time:
            raise ValueError("available_at must not precede event_time")
        return self


class ImportRequest(BaseModel):
    command_id: str = Field(min_length=1, max_length=100)
    source: str = Field(min_length=1, max_length=200)
    csv: str = Field(min_length=1, max_length=2_000_000)


def encode_csv(content: str) -> tuple[bytes, dict]:
    bars = [Bar.model_validate(row) for row in csv.DictReader(io.StringIO(content))]
    if not bars:
        raise ValueError("Source returned no rows; no snapshot published")
    keys = [(bar.contract, bar.event_time) for bar in bars]
    if len(keys) != len(set(keys)):
        raise ValueError("Duplicate contract/event_time keys")
    bars.sort(key=lambda b: (b.contract, b.event_time))
    schema = pa.schema(
        [
            ("contract", pa.string()),
            ("event_time", pa.timestamp("us", tz="UTC")),
            ("available_at", pa.timestamp("us", tz="UTC")),
            ("trading_day", pa.date32()),
            *[(name, pa.decimal128(20, 8)) for name in ("open", "high", "low", "close")],
            ("volume", pa.int64()),
        ]
    )
    table = pa.Table.from_pylist([b.model_dump() for b in bars], schema=schema)
    output = pa.BufferOutputStream()
    pq.write_table(table, output)
    data = output.getvalue().to_pybytes()
    return data, {
        "schema_version": 1,
        "rows": len(bars),
        "checksum": hashlib.sha256(data).hexdigest(),
        "contracts": sorted({b.contract for b in bars}),
        "start": min(b.event_time for b in bars).isoformat(),
        "end": max(b.event_time for b in bars).isoformat(),
    }


def read_bars(path, limit=1000):
    parquet = pq.ParquetFile(path)
    batch = next(parquet.iter_batches(batch_size=limit))
    rows = batch.to_pylist()
    return [
        {k: str(v) if isinstance(v, (Decimal, datetime, date)) else v for k, v in row.items()}
        for row in rows
    ]


def initialize_catalog(engine):
    from asterion.data.catalog import snapshots

    snapshots.create(engine, checkfirst=True)
