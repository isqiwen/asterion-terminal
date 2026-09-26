"""Local capability binding retains typed ports and declared dependency checks."""

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import fields, is_dataclass, replace
from functools import wraps
from inspect import iscoroutinefunction
from pathlib import PurePath

from asterion_bindings import _native
from asterion_bindings.communication import activate, context
from asterion_bindings.storage import Storage

_lifetimes: ContextVar[tuple[_native.HostHandle | _native.ExecutionHandle, ...]] = ContextVar(
    "capability_lifetimes", default=()
)


def current_lifetimes():
    """Capture native monotonic scopes for a result consumed after dispatch returns."""
    return _lifetimes.get()


@contextmanager
def _lifetime(handle):
    token = _lifetimes.set((*_lifetimes.get(), handle)) if handle is not None else None
    try:
        yield
    finally:
        if token is not None:
            _lifetimes.reset(token)


def bind(port, *, guard=None, lifetime=None):
    def wrap(function):
        if iscoroutinefunction(function):

            @wraps(function)
            async def asynchronous(*args, **kwargs):
                if guard is not None:
                    guard()
                with _lifetime(lifetime), activate(context()):
                    return await function(*args, **kwargs)

            return asynchronous

        @wraps(function)
        def synchronous(*args, **kwargs):
            if guard is not None:
                guard()
            with _lifetime(lifetime), activate(context()):
                return function(*args, **kwargs)

        return synchronous

    if callable(port) and not isinstance(port, type):
        return wrap(port)
    if lifetime is not None and isinstance(port, Storage):
        return port._guarded(lifetime)
    if not is_dataclass(port) or isinstance(port, type):
        if (
            guard is not None
            and not isinstance(port, (str, bytes, int, float, bool, PurePath, type))
            and port is not None
        ):
            return _ResourceView(port, guard, lifetime)
        return port

    return replace(
        port,
        **{
            field.name: wrap(getattr(port, field.name))
            for field in fields(port)
            if callable(getattr(port, field.name))
        },
    )


class _ResourceView:
    """Gate captured methods as well as future attribute reads on object ports."""

    def __init__(self, value, guard, lifetime):
        self._value, self._guard, self._lifetime = value, guard, lifetime

    def __getattr__(self, name):
        self._guard()
        value = getattr(self._value, name)
        return bind(value, guard=self._guard, lifetime=self._lifetime) if callable(value) else value
