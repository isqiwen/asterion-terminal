"""Public contract for trusted task contributions; no business-module dependencies."""

import re
from collections.abc import Callable, Iterable
from dataclasses import dataclass

from asterion.platform.authorization import Grant
from asterion.platform.registry import Registry
from asterion.platform.resource import Resource
from asterion.platform.tasks.execution import ExecutionContext

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

    def __post_init__(self):
        if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", self.kind):
            raise ValueError("Task handler kind must be a namespaced ID")
        for grant in self.requests:
            if (
                not grant.path.startswith("/jobs/:id/")
                or grant.methods != ("POST",)
                or grant.descendants
            ):
                raise ValueError("Worker requests must be exact job-relative POST operations")
        if not re.fullmatch(r"/[a-z][a-z0-9-]*", self.publish_suffix):
            raise ValueError("Task handler must publish to a job-relative endpoint")


class TaskHandlerRegistry:
    def __init__(self, handlers: Iterable[TaskHandler] = ()):
        self._handlers: Registry[TaskHandler] = Registry()
        for handler in handlers:
            self.register(handler)

    def register(self, handler: TaskHandler) -> None:
        try:
            self._handlers.register(handler.kind, handler)
        except ValueError as exc:
            raise ValueError(f"Duplicate task handler or invalid ID: {handler.kind}") from exc

    def get(self, kind: str) -> TaskHandler:
        try:
            return self._handlers.get(kind)
        except KeyError:
            raise ValueError(f"Unsupported job kind: {kind}") from None
