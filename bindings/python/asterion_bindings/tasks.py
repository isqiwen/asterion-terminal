"""Value/object adaptation for Rust task rules and execution resource handles."""

import json
from collections.abc import Mapping
from types import MappingProxyType
from typing import cast

from asterion_bindings.local import bind
from asterion_bindings.resource import Resource

from . import _native
from ._call import invoke


def task_rule(operation, **value):
    return invoke("kernel", f"tasks.{operation}", value)


def _resource(resource):
    return {
        "id": resource.id,
        "contract": f"{resource.contract.__module__}.{resource.contract.__qualname__}",
    }


class ExecutionContext:
    def __init__(self, declarations: tuple[Resource, ...], bindings: Mapping[Resource, object]):
        self._scope = _native.ExecutionScope(
            json.dumps([_resource(resource) for resource in declarations]),
            json.dumps([_resource(resource) for resource in bindings]),
        )
        try:
            for resource, value in bindings.items():
                if not isinstance(value, resource.contract):
                    raise TypeError(f"Invalid execution resource: {resource.id}")
            self._resources = MappingProxyType(dict(bindings))
            self._ports = {}
        except BaseException:
            self._scope.close()
            raise

    def resource[T](self, resource: Resource[T]) -> T:
        handle = self._scope.resource(json.dumps(_resource(resource)))
        value = self._resources[resource]
        if resource not in self._ports:
            self._ports[resource] = bind(value, guard=handle.check, lifetime=handle)
        return cast(T, self._ports[resource])

    def close(self):
        self._scope.close()
        self._ports.clear()
        self._resources = MappingProxyType({})
