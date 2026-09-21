"""Per-invocation resource access without runtime configuration or lease credentials."""

from collections.abc import Mapping
from types import MappingProxyType
from typing import cast

from asterion.platform.resource import Resource


class ExecutionContext:
    def __init__(self, declarations: tuple[Resource, ...], bindings: Mapping[Resource, object]):
        if len({resource.id for resource in declarations}) != len(declarations):
            raise ValueError("Duplicate execution resource")
        if set(declarations) != set(bindings):
            raise ValueError("Execution resources do not match grants")
        for resource, value in bindings.items():
            if not isinstance(value, resource.contract):
                raise TypeError(f"Invalid execution resource: {resource.id}")
        self._resources = MappingProxyType(dict(bindings))
        self._closed = False

    def resource[T](self, resource: Resource[T]) -> T:
        if self._closed:
            raise ValueError("Execution context is closed")
        if resource not in self._resources:
            raise ValueError(f"Execution resource is not granted: {resource.id}")
        return cast(T, self._resources[resource])

    def close(self):
        self._closed = True
        self._resources = MappingProxyType({})
