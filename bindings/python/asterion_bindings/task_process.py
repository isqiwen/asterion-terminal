"""Python values and binary HTTP iteration for Rust-owned computation processes."""

import json

from . import _native
from .diagnostics import ProcessFailure


def _failure(error):
    return ProcessFailure(*error.args)


class TaskArtifact:
    def __init__(self, native):
        self._native = native

    @property
    def metadata(self):
        return json.loads(self._native.metadata())

    def __iter__(self):
        while chunk := self._native.read_chunk():
            yield chunk

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._native.close()


class TaskProcess:
    def __init__(self, arguments, context, request, *, limits):
        try:
            self._native = _native.TaskProcess(
                list(map(str, arguments)),
                json.dumps(context, allow_nan=False),
                json.dumps(request, ensure_ascii=False, allow_nan=False).encode(),
                json.dumps(limits, allow_nan=False),
            )
        except ValueError as error:
            raise _failure(error) from None

    def poll(self, timeout):
        try:
            return self._native.poll(timeout)
        except ValueError as error:
            raise _failure(error) from None

    def take_result(self):
        try:
            return TaskArtifact(self._native.take_result())
        except ValueError as error:
            raise _failure(error) from None

    def close(self):
        self._native.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def serve(dispatch):
    def callback(raw):
        try:
            content, metadata = dispatch(json.loads(raw))
            if not isinstance(content, bytes) or not isinstance(metadata, dict):
                raise TypeError("Invalid task result")
            return content, json.dumps(metadata, ensure_ascii=False, allow_nan=False), None
        except ValueError as error:
            message = str(error)
            return b"", "{}", message if len(message.encode()) <= 2000 else "Task execution failed"
        except Exception:  # noqa: BLE001 - do not forward arbitrary Python exception objects
            return b"", "{}", "Task execution failed"

    try:
        _native.task_process_serve(callback)
    except ValueError as error:
        raise _failure(error) from None
