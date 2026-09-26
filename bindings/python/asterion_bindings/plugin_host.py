"""Python callbacks and framework adaptation for the fixed Rust plugin host.

Rust owns declaration/graph/grant checks, lifecycle and handle liveness. This
binding retains Python object type checks and executes approved callbacks; it
does not choose providers or implement a second host state machine.
"""

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import cast

from asterion_bindings.events import EventPort, Topic
from asterion_bindings.local import bind
from asterion_bindings.recovery import BackupCheck, RestoreStep
from asterion_bindings.resource import Resource
from asterion_bindings.task_handlers import TaskHandler, TaskHandlerRegistry

from . import _native


@dataclass(frozen=True)
class Capability[T]:
    id: str
    provider: str
    contract: type[T]


@dataclass(frozen=True)
class Activation:
    exports: dict[Capability, object] = field(default_factory=dict)
    routers: tuple = ()
    exception_handlers: tuple[tuple[type[Exception], Callable], ...] = ()
    hooks: dict[str, tuple[Callable, ...]] = field(default_factory=dict)
    close: Callable[[], None] = lambda: None


@dataclass(frozen=True)
class Plugin:
    id: str
    requires: tuple[str, ...]
    activate: Callable[["Context"], Activation]
    handlers: tuple[TaskHandler, ...] = ()
    backup: BackupCheck | None = None
    restore: RestoreStep | None = None
    api_version: int = 1
    provides: tuple[Capability, ...] = ()
    consumes: tuple[Capability, ...] = ()
    observes: tuple[str, ...] = ()
    resources: tuple[Resource, ...] = ()
    publishes: tuple[Topic, ...] = ()
    # Python callback contributions are L3. Remaining monolithic feature code
    # still requires the documented domain/UI split; this flag is not migration.
    layer: str = "L3"
    type_requires: tuple[str, ...] = ()


@dataclass(frozen=True)
class Context:
    plugin: Plugin
    _resources: Callable[[Resource], object]
    _resolve: Callable[[Capability], object]
    _hooks: Callable[[str], tuple[Callable, ...]]
    _publisher: EventPort | None = None

    def publish(self, transaction, topic, stream, payload):
        if self._publisher is None:
            raise ValueError("Event port unavailable")
        return self._publisher.publish(transaction, topic, stream, payload)

    def resource[T](self, resource: Resource[T]) -> T:
        return cast(T, self._resources(resource))

    def require[T](self, capability: Capability[T]) -> T:
        return cast(T, self._resolve(capability))

    def hooks(self, name: str):
        return self._hooks(name)


def _contract(contract):
    return f"{contract.__module__}.{contract.__qualname__}"


def _capability(capability):
    return {
        "id": capability.id,
        "provider": capability.provider,
        "contract": _contract(capability.contract),
    }


def _resource(resource):
    return {"id": resource.id, "contract": _contract(resource.contract)}


def _manifest(plugin):
    if plugin.layer != "L3":
        raise ValueError("Python callback plugins must belong to L3")
    return {
        "id": plugin.id,
        "api_version": plugin.api_version,
        "layer": plugin.layer,
        "type_requires": plugin.type_requires,
        "requires": plugin.requires,
        "provides": [_capability(port) for port in plugin.provides],
        "consumes": [_capability(port) for port in plugin.consumes],
        "resources": [_resource(resource) for resource in plugin.resources],
        "observes": plugin.observes,
    }


class PluginHost:
    def __init__(self, plugins: tuple[Plugin, ...]):
        self._native = _native.PluginHost(json.dumps([_manifest(plugin) for plugin in plugins]))
        self._plugins = {plugin.id: plugin for plugin in plugins}
        self.plugins = tuple(self._plugins[identifier] for identifier in self._native.order)
        self._active: dict[str, Activation] = {}
        self.handlers = TaskHandlerRegistry(
            handler for plugin in self.plugins for handler in plugin.handlers
        )

    def activate(self, app, bindings, *, events=None):
        # Rust validates the complete resource grant set before any callback.
        self._native.begin(
            json.dumps(
                {
                    identifier: [_resource(resource) for resource in values]
                    for identifier, values in bindings.items()
                }
            )
        )
        try:
            granted = {}
            for plugin in self.plugins:
                values = dict(bindings.get(plugin.id, {}))
                for resource, value in values.items():
                    if not isinstance(value, resource.contract):
                        raise TypeError(f"Invalid resource value: {plugin.id} -> {resource.id}")
                granted[plugin.id] = MappingProxyType(values)

            def resources_for(identifier):
                retained = {}

                def resource_value(resource):
                    handle = self._native.resource(identifier, json.dumps(_resource(resource)))
                    handle.check()
                    if resource not in retained:
                        retained[resource] = bind(
                            granted[identifier][resource], guard=handle.check, lifetime=handle
                        )
                    return retained[resource]

                return resource_value

            def resolve_for(identifier):
                def resolve(capability):
                    handle = self._native.capability(
                        identifier, json.dumps(_capability(capability))
                    )
                    return bind(
                        self._active[capability.provider].exports[capability],
                        guard=handle.check,
                        lifetime=handle,
                    )

                return resolve

            def hooks_for(identifier):
                def hooks(name):
                    handle = self._native.observation(identifier, name)
                    handle.check()
                    return self.hooks(name)

                return hooks

            while (identifier := self._native.next_activation()) is not None:
                plugin = self._plugins[identifier]
                activation = plugin.activate(
                    Context(
                        plugin,
                        resources_for(identifier),
                        resolve_for(identifier),
                        hooks_for(identifier),
                        events.publisher(plugin.id, plugin.publishes)._guarded(
                            self._native.context(identifier)
                        )
                        if events
                        else None,
                    )
                )
                if not isinstance(activation, Activation):
                    raise TypeError(f"Invalid activation: {plugin.id}")
                self._active[identifier] = activation
                self._native.returned(identifier)
                try:
                    exports = [_capability(port) for port in activation.exports]
                except (AttributeError, TypeError) as error:
                    raise TypeError(f"Invalid capability export: {plugin.id}") from error
                self._native.activated(identifier, json.dumps(exports))
                for capability, value in activation.exports.items():
                    if not isinstance(value, capability.contract):
                        raise TypeError(f"Invalid capability value: {capability.id}")

            handlers = {}
            for activation in self._active.values():
                for exception, handler in activation.exception_handlers:
                    if exception in handlers or exception in app.exception_handlers:
                        raise ValueError(f"Duplicate exception handler: {exception.__name__}")
                    handlers[exception] = handler
            self._native.finish()
            for exception, handler in handlers.items():
                app.add_exception_handler(exception, handler)
            for activation in self._active.values():
                for router in activation.routers:
                    app.include_router(router)
        except BaseException:
            self.close()
            raise

    def resolve[T](self, capability: Capability[T]) -> T:
        handle = self._native.export(json.dumps(_capability(capability)))
        return cast(
            T,
            bind(
                self._active[capability.provider].exports[capability],
                guard=handle.check,
                lifetime=handle,
            ),
        )

    def hooks(self, name: str):
        result = []
        for identifier, activation in self._active.items():
            handle = self._native.plugin(identifier)
            result.extend(
                bind(hook, guard=handle.check, lifetime=handle)
                for hook in activation.hooks.get(name, ())
            )
        return tuple(result)

    def close(self):
        errors = []
        for identifier in self._native.close():
            try:
                self._active[identifier].close()
            except Exception as error:  # noqa: BLE001 - every callback must be released
                errors.append(error)
        self._active.clear()
        if errors:
            raise ExceptionGroup("Plugin shutdown failed", errors)
