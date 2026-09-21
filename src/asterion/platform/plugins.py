"""Lifecycle and dependency mechanisms for explicitly approved, in-process plugins."""

import re
from collections.abc import Callable
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import cast

from asterion.platform.backup import BackupCheck, RestoreStep
from asterion.platform.resource import Resource
from asterion.platform.tasks.handlers import TaskHandler, TaskHandlerRegistry


@dataclass(frozen=True)
class Capability[T]:
    """A named, typed port owned by one declared plugin."""

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


@dataclass(frozen=True)
class Context:
    """Trusted in-process bootstrap resources plus declared collaboration ports.

    Bootstrap resources are not a sandbox or an external plugin SDK.
    """

    plugin: Plugin
    _resources: Callable[[Resource], object]
    _resolve: Callable[[Capability], object]
    _hooks: Callable[[str], tuple[Callable, ...]]

    def resource[T](self, resource: Resource[T]) -> T:
        if resource not in self.plugin.resources:
            raise ValueError(f"Undeclared resource: {self.plugin.id} -> {resource.id}")
        return cast(T, self._resources(resource))

    def require[T](self, capability: Capability[T]) -> T:
        if capability not in self.plugin.consumes:
            raise ValueError(f"Undeclared capability: {self.plugin.id} -> {capability.id}")
        return cast(T, self._resolve(capability))

    def hooks(self, name: str) -> tuple[Callable, ...]:
        if name not in self.plugin.observes:
            raise ValueError(f"Undeclared hook observation: {self.plugin.id} -> {name}")
        return self._hooks(name)


class PluginHost:
    def __init__(self, plugins: tuple[Plugin, ...]):
        self._plugins: dict[str, Plugin] = {}
        self._active: dict[str, Activation] = {}
        for plugin in plugins:
            if plugin.api_version != 1:
                raise ValueError(f"Unsupported plugin contract: {plugin.id}")
            if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", plugin.id):
                raise ValueError(f"Invalid plugin ID: {plugin.id}")
            if plugin.id in self._plugins:
                raise ValueError(f"Duplicate plugin: {plugin.id}")
            self._plugins[plugin.id] = plugin
        ordered: list[Plugin] = []
        visiting: set[str] = set()
        visited: set[str] = set()

        def visit(identifier):
            if identifier in visiting:
                raise ValueError(f"Plugin dependency cycle: {identifier}")
            if identifier in visited:
                return
            if identifier not in self._plugins:
                raise ValueError(f"Missing required plugin: {identifier}")
            visiting.add(identifier)
            plugin = self._plugins[identifier]
            for dependency in plugin.requires:
                visit(dependency)
            visiting.remove(identifier)
            visited.add(identifier)
            ordered.append(plugin)

        for identifier in self._plugins:
            visit(identifier)
        self.plugins = tuple(ordered)
        capabilities: dict[str, Capability] = {}
        for plugin in self._plugins.values():
            for capability in plugin.provides:
                if capability.provider != plugin.id:
                    raise ValueError(f"Capability owner mismatch: {capability.id}")
                if capability.id in capabilities:
                    raise ValueError(f"Duplicate capability: {capability.id}")
                capabilities[capability.id] = capability
        for plugin in self._plugins.values():
            for capability in plugin.consumes:
                if capability.provider not in plugin.requires:
                    raise ValueError(f"Undeclared provider dependency: {capability.id}")
                if capabilities.get(capability.id) != capability:
                    raise ValueError(f"Missing or mismatched capability: {capability.id}")
        # Check all task contributions before invoking any plugin activation code.
        self.handlers = TaskHandlerRegistry(h for p in self.plugins for h in p.handlers)
        self._started = False
        self._closed = False

    def activate(self, app, bindings):
        if self._started or self._closed:
            raise ValueError("Plugin host cannot be activated twice")
        self._started = True
        try:
            # Validate the complete grant set before running any activation code.
            if set(bindings) - set(self._plugins):
                raise ValueError("Resource grants name unknown plugins")
            granted = {}
            for plugin in self.plugins:
                values = dict(bindings.get(plugin.id, {}))
                if len({r.id for r in plugin.resources}) != len(plugin.resources):
                    raise ValueError(f"Duplicate resource declaration: {plugin.id}")
                if set(values) != set(plugin.resources):
                    raise ValueError(f"Resource grants do not match declaration: {plugin.id}")
                for resource, value in values.items():
                    if not isinstance(value, resource.contract):
                        raise TypeError(f"Invalid resource value: {plugin.id} -> {resource.id}")
                granted[plugin.id] = MappingProxyType(values)

            def resources_for(identifier):
                def resolve(resource):
                    if self._closed:
                        raise ValueError("Plugin host is closed")
                    return granted[identifier][resource]

                return resolve

            for plugin in self.plugins:
                activation = plugin.activate(
                    Context(plugin, resources_for(plugin.id), self.resolve, self.hooks)
                )
                if not isinstance(activation, Activation):
                    raise TypeError(f"Invalid activation: {plugin.id}")
                self._active[plugin.id] = activation
                if set(activation.exports) != set(plugin.provides):
                    raise ValueError(f"Capability exports do not match declaration: {plugin.id}")
                for capability, value in activation.exports.items():
                    if not isinstance(value, capability.contract):
                        raise TypeError(f"Invalid capability value: {capability.id}")
            handlers = {}
            for activation in self._active.values():
                for exception, handler in activation.exception_handlers:
                    if exception in handlers or exception in app.exception_handlers:
                        raise ValueError(f"Duplicate exception handler: {exception.__name__}")
                    handlers[exception] = handler
            for exception, handler in handlers.items():
                app.add_exception_handler(exception, handler)
            # No routes become visible until every required activation succeeds.
            for activation in self._active.values():
                for router in activation.routers:
                    app.include_router(router)
        except BaseException:
            self.close()
            raise

    def resolve[T](self, capability: Capability[T]) -> T:
        if capability.provider not in self._active:
            raise ValueError(f"Plugin is not active: {capability.provider}")
        exports = self._active[capability.provider].exports
        if capability not in exports:
            raise ValueError(f"Capability is not available: {capability.id}")
        return cast(T, exports[capability])

    def hooks(self, name: str):
        return tuple(hook for value in self._active.values() for hook in value.hooks.get(name, ()))

    def close(self):
        if self._closed:
            return
        self._closed = True
        errors = []
        for activation in reversed(tuple(self._active.values())):
            try:
                activation.close()
            except Exception as error:  # noqa: BLE001 - all plugins must be released
                errors.append(error)
        self._active.clear()
        if errors:
            raise ExceptionGroup("Plugin shutdown failed", errors)
