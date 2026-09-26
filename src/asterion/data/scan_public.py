"""Application result view over the authoritative L2 scan request and Arrow stream."""

from collections.abc import Iterator
from dataclasses import dataclass
from typing import Protocol

import pyarrow as pa
from asterion_bindings.data_store import ScanRequest

__all__ = ["ScanRequest", "ScanResult"]


class Batches(Protocol):
    def __iter__(self) -> Iterator[pa.RecordBatch]: ...
    def __next__(self) -> pa.RecordBatch: ...
    def close(self) -> None: ...


@dataclass(frozen=True)
class ScanResult:
    version: dict
    # Manifest partition order, then stored row order. Consumers can explicitly
    # sort a bounded selection; closing also releases an unstarted native scan.
    batches: Batches
