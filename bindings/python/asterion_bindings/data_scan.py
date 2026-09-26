"""Host construction and Arrow ownership for the native fixed-version scanner."""

import json
from collections.abc import Iterator
from pathlib import Path

import pyarrow as pa

from . import _native
from .data_store import ScanRequest
from .local import current_lifetimes


def open_scan(
    root: Path,
    scope: _native.StorageScope,
    version: dict,
    request: ScanRequest,
) -> _native.NativeDataScan:
    files = _native.ReadFilesHandle(root, 1024**3, 1_000_000)
    return _native.NativeDataScan(
        files,
        scope,
        list(current_lifetimes()),
        json.dumps(version, ensure_ascii=False, allow_nan=False),
        request.model_dump_json(),
    )


class ScanBatches(Iterator[pa.RecordBatch]):
    """Explicit ownership: closing an unstarted scan still releases its grant."""

    def __init__(self, scan: _native.NativeDataScan) -> None:
        self._scan = scan

    def __iter__(self) -> "ScanBatches":
        return self

    def __next__(self) -> pa.RecordBatch:
        try:
            batch = self._scan.next_batch()
        except BaseException:
            self.close()
            raise
        if batch is None:
            raise StopIteration
        return batch

    def close(self) -> None:
        self._scan.close()
