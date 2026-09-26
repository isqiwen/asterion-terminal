"""Data source connections and configurations of the Rust data service, on the
caller's storage transaction. The runtime key stays inside the native handle;
refusals raise ValueError and concurrent changes Conflict."""

import json
from pathlib import Path

from . import _native
from .database import kernel_call, native_connection
from .storage import transaction_connection


def _call(transaction, action, *args, write=False):
    conn = transaction_connection(transaction, write=write)
    return json.loads(kernel_call(conn, action, native_connection(conn), *args))


class SourceCredentials:
    """Encrypted configuration snapshots under one data root."""

    def __init__(self, master: str, root: Path):
        self._handle = _native.DataCredentials(master, root)

    def fix_for_task(self, transaction, sources: list[dict], owner: str, provider: str) -> dict:
        """`{"configuration": fixed reference, "connection_name": ...}` for a new task."""
        return _call(transaction, self._handle.fix_for_task, json.dumps(sources), owner, provider)

    def resolve(self, fixed, owner: str, spec: dict) -> dict:
        """The runnable values a task was fixed to."""
        return json.loads(self._handle.resolve(json.dumps(fixed), owner, json.dumps(spec)))

    def state(self, transaction, sources: list[dict], owner: str) -> dict:
        return _call(transaction, self._handle.state, json.dumps(sources), owner)

    def apply(self, transaction, sources: list[dict], owner: str, update: dict) -> dict:
        return _call(
            transaction,
            self._handle.apply,
            json.dumps(sources),
            owner,
            json.dumps(update),
            write=True,
        )

    def listing(self, transaction, sources: list[dict]) -> list[dict]:
        return _call(transaction, self._handle.listing, json.dumps(sources))

    def opens(self, sealed: bytes) -> bool:
        return self._handle.opens(sealed)


def connection_state(transaction, sources: list[dict], identifier: str) -> dict:
    return _call(transaction, _native.data_connection_state, json.dumps(sources), identifier)


def connection_create(transaction, sources: list[dict], provider: str, name: str) -> dict:
    body = json.dumps({"provider": provider, "name": name})
    return _call(transaction, _native.data_connection_create, json.dumps(sources), body, write=True)


def connection_update(transaction, sources: list[dict], identifier: str, update: dict) -> dict:
    return _call(
        transaction,
        _native.data_connection_update,
        json.dumps(sources),
        identifier,
        json.dumps(update),
        write=True,
    )
