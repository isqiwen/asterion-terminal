"""Bounded, cross-process execution observations; never persist call data or exceptions."""

import re
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path
from uuid import uuid4


class ProcessFailure(ValueError):
    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(message)


@contextmanager
def connection(root: Path):
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    conn = sqlite3.connect(root / "diagnostics.sqlite", timeout=2)
    try:
        with conn:
            conn.execute(
                "CREATE TABLE IF NOT EXISTS executions (id TEXT PRIMARY KEY, digest TEXT NOT NULL, started REAL NOT NULL, duration_ms INTEGER NOT NULL, phase TEXT NOT NULL, code TEXT NOT NULL, calls INTEGER NOT NULL)"
            )
            yield conn
    finally:
        conn.close()


def recent(root: Path, digest: str):
    with connection(root) as conn:
        conn.row_factory = sqlite3.Row
        return [
            dict(row)
            for row in conn.execute(
                "SELECT * FROM executions WHERE digest=? ORDER BY started DESC LIMIT 30", (digest,)
            )
        ]


class Observation:
    def __init__(self, path: Path):
        self.root = path.parent.parent
        self.digest = path.name
        self.started = time.time()
        self.clock = time.monotonic()
        self.phase = "startup"
        self.calls = 0
        self.finished = False

    def call(self, method):
        self.phase = method if re.fullmatch(r"[a-z][a-z0-9_.]{0,63}", method) else "request"
        self.calls += 1

    def finish(self, code="success"):
        if self.finished:
            return
        self.finished = True
        if not re.fullmatch(r"[0-9a-f]{64}", self.digest):
            return
        # Diagnostics must never change the result of execution or publish secrets.
        try:
            with connection(self.root) as conn:
                conn.execute(
                    "INSERT INTO executions VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (
                        uuid4().hex,
                        self.digest,
                        self.started,
                        round((time.monotonic() - self.clock) * 1000),
                        self.phase,
                        code,
                        self.calls,
                    ),
                )
                conn.execute(
                    "DELETE FROM executions WHERE id NOT IN (SELECT id FROM executions ORDER BY started DESC LIMIT 200)"
                )
        except (OSError, sqlite3.Error):
            pass
