"""Product process declarations are values; Rust owns process lifetime and supervision."""

import json
from pathlib import Path

from . import _native


class Supervisor:
    def __init__(self, processes: list[dict], status_file: Path, build_id: str):
        self._native = _native.Supervisor(
            json.dumps(
                {
                    "processes": processes,
                    "status_file": str(status_file),
                    "build_id": build_id,
                    "policy": {
                        "restart_millis": 3000,
                        "snapshot_millis": 500,
                        "shutdown_millis": 10000,
                    },
                },
                allow_nan=False,
            )
        )

    def request_stop(self) -> None:
        self._native.request_stop()

    def run(self) -> None:
        self._native.run()
