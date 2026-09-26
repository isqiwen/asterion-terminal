"""Python values and callback adaptation for the fixed host environment mechanism."""

import json
from collections.abc import Callable
from pathlib import Path

from . import _native


class EnvironmentLease:
    def __init__(self, host: Path, backup_directory: Path, layout: dict, *, wait: bool = False):
        self._native = _native.EnvironmentLease(host, backup_directory, json.dumps(layout), wait)

    def close(self) -> None:
        self._native.close()

    def active(self) -> Path:
        return self._native.active()

    def status(self) -> dict:
        return json.loads(self._native.status())

    @staticmethod
    def _callback(callback: Callable[[dict], object]) -> Callable[[str], str]:
        return lambda raw: json.dumps(callback(json.loads(raw)), allow_nan=False)

    def recover(self, callback: Callable[[dict], object]) -> None:
        self._native.recover(self._callback(callback))

    def switch(self, target: Path | None, callback: Callable[[dict], object]) -> dict:
        return json.loads(self._native.switch(target, self._callback(callback)))
