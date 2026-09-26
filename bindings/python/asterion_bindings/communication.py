"""Python context storage and value conversion for fixed Rust communication."""

import json
from contextlib import contextmanager
from contextvars import ContextVar

from . import _native

_current: ContextVar[str | None] = ContextVar("communication_context", default=None)


def validate(name, value):
    _native.communication_validate(name, value)
    return value


def loads(raw):
    if isinstance(raw, (bytes, bytearray)):
        raw = raw.decode("utf-8")
    return json.loads(_native.communication_loads(raw))


def context(timeout: float = 30):
    return json.loads(_native.communication_context(timeout, _current.get()))


def continuation(correlation_id: str, causation_id: str, timeout: float):
    return json.loads(_native.communication_continue(correlation_id, causation_id, timeout))


@contextmanager
def activate(value):
    # The async task-local representation is immutable. Mutating a caller's dict
    # cannot extend the active context or change correlation for another task.
    encoded = _native.communication_active(value)
    token = _current.set(encoded)
    try:
        yield json.loads(encoded)
    finally:
        _current.reset(token)


def current():
    encoded = _current.get()
    return json.loads(encoded) if encoded is not None else None


def ingress(raw, timeout: float = 60):
    if isinstance(raw, (bytes, bytearray)):
        raw = raw.decode("utf-8")
    return json.loads(_native.communication_ingress(raw, timeout))


def call(contract, payload, *, kind="command", timeout: float = 30):
    return json.loads(_native.communication_call(timeout, _current.get(), kind, contract, payload))


def reply(request, result=None, error=None):
    return json.loads(_native.communication_reply(request["context"], result, error))


def result(request, response):
    return json.loads(_native.communication_result(request["context"], response))
