"""Value and callback adapters for the fixed Rust process transport."""

import json

from . import _native
from .diagnostics import ProcessFailure


def _failure(error):
    code, message = error.args
    if code == "revoked":
        message = "插件已停用或移除"
    return ProcessFailure(code, message)


def invoke(arguments, request, *, cwd=None, authorized=None):
    try:
        raw = _native.transport_invoke(
            list(map(str, arguments)),
            json.dumps(request, allow_nan=False),
            str(cwd) if cwd is not None else None,
            authorized,
        )
    except ValueError as error:
        if len(error.args) != 2:
            raise ProcessFailure("protocol", "插件请求不符合当前通信契约") from None
        raise _failure(error) from None
    return json.loads(raw)


class ProcessTransport:
    def __init__(self, arguments, *, cwd, timeout, authorized):
        try:
            self._native = _native.ProcessTransport(
                list(map(str, arguments)), str(cwd), timeout, authorized
            )
        except ValueError as error:
            raise _failure(error) from None

    def call(self, request):
        try:
            raw = self._native.call(json.dumps(request, allow_nan=False))
            return json.loads(raw)
        except BaseException as error:
            self.close()
            if isinstance(error, ValueError) and len(error.args) == 2:
                raise _failure(error) from None
            raise

    def close(self):
        self._native.close()


def serve(dispatch):
    def callback(raw):
        value = dispatch(json.loads(raw))
        return json.dumps(value, ensure_ascii=False, allow_nan=False)

    try:
        _native.transport_serve(callback)
    except ValueError as error:
        raise _failure(error) from None


def child_limits():
    try:
        _native.transport_child_limits()
    except ValueError as error:
        raise _failure(error) from None
