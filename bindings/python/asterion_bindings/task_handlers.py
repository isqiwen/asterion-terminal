"""Python callback objects and HTTP values for Rust task declarations.

These are trusted host bindings, not a sandbox for arbitrary Python code.
"""

import json
from collections.abc import Callable, Iterable
from dataclasses import asdict, dataclass, field

from asterion_bindings.resource import Resource

from . import _native
from .authority import Grant
from .tasks import ExecutionContext

Executor = Callable[[ExecutionContext, dict], tuple[bytes, dict]]


@dataclass(frozen=True)
class PublicationResult:
    status_code: int
    detail: object = None

    def raise_for_status(self):
        if not 200 <= self.status_code < 300:
            raise ValueError(f"Publication failed (HTTP {self.status_code})")


PublicationCheck = Callable[[PublicationResult], None]


def check_publication(response: PublicationResult) -> None:
    response.raise_for_status()


@dataclass(frozen=True)
class TaskHandler:
    kind: str
    execute: Executor
    publish_suffix: str
    check_publication: PublicationCheck = check_publication
    resources: tuple[Resource, ...] = ()
    requests: tuple[Grant, ...] = ()
    _declaration: _native.TaskHandlerDeclaration = field(init=False, repr=False, compare=False)

    def __post_init__(self):
        if not callable(self.execute) or not callable(self.check_publication):
            raise TypeError("Task handler callbacks must be callable")
        if any(not isinstance(resource, Resource) for resource in self.resources):
            raise TypeError("Task handler resources must be Resource values")
        if any(not isinstance(grant, Grant) for grant in self.requests):
            raise TypeError("Task handler requests must be Grant values")
        object.__setattr__(
            self,
            "_declaration",
            _native.TaskHandlerDeclaration(
                self.kind,
                self.publish_suffix,
                json.dumps([asdict(grant) for grant in self.requests]),
            ),
        )

    def request_scope(self):
        return self._declaration.requests()


class TaskHandlerRegistry:
    def __init__(self, handlers: Iterable[TaskHandler] = ()):
        self._native = _native.TaskHandlerRegistry()
        self._callbacks: list[TaskHandler] = []
        for handler in handlers:
            self.register(handler)

    def register(self, handler: TaskHandler) -> None:
        if not isinstance(handler, TaskHandler):
            raise TypeError("Task handlers must be TaskHandler values")
        self._native.register(handler._declaration)
        self._callbacks.append(handler)

    def get(self, kind: str) -> TaskHandler:
        return self._callbacks[self._native.get(kind)]

    def worker_grants(self) -> tuple[Grant, ...]:
        return tuple(
            Grant(value["path"], tuple(value["methods"]), value["descendants"])
            for value in json.loads(self._native.worker_grants())
        )
