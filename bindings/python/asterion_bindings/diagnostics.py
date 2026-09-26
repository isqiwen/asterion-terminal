"""Python value adaptation for native best-effort execution diagnostics."""

import json
from pathlib import Path

from . import _native
from .communication import current

_CURRENT = object()


class ProcessFailure(ValueError):
    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(message)


class Observation:
    def __init__(self, path: Path):
        self._native = _native.DiagnosticsObservation(path.parent.parent, path.name)

    def call(self, method: str) -> None:
        self._native.call(method)

    def finish(self, code: str = "success") -> None:
        self._native.finish(code)


def recent(root: Path, digest: str) -> list[dict]:
    return json.loads(_native.diagnostics_recent(root, digest))


def record_event(root: Path, component: str, code: str, *, context=_CURRENT) -> None:
    trace = current() if context is _CURRENT else context
    _native.diagnostic_event(root, component, code, trace)


def events(root: Path) -> list[dict]:
    return json.loads(_native.diagnostics_events(root))
