from dataclasses import replace

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.plugin_host import Activation, Capability, Plugin, PluginHost
from asterion_bindings.resource import Resource
from fastapi import APIRouter, FastAPI
from fastapi.testclient import TestClient
from storage_support import raw_engine

from asterion.api.app import create_app
from asterion.distribution import builtin_plugins
from asterion.platform.config import Settings


def test_dependency_validation_happens_before_activation():
    calls = []
    base = Plugin("fixture.base", (), lambda context: calls.append(context))
    for entries, match in [
        ((base, base), "Duplicate plugin"),
        ((replace(base, requires=("fixture.missing",)),), "Missing required"),
        ((replace(base, requires=(base.id,)),), "dependency cycle"),
        ((replace(base, api_version=-1),), "Unsupported plugin contract"),
    ]:
        with pytest.raises(ValueError, match=match):
            PluginHost(entries)
    assert calls == []


def test_object_resources_captured_methods_and_lazy_scopes_are_revoked():
    from asterion_bindings.local import current_lifetimes

    class Source:
        def capture(self):
            return current_lifetimes()

    resource = Resource("fixture.source", Source)
    retained = []

    def activate(context):
        retained.append(context.resource(resource))
        return Activation()

    host = PluginHost((Plugin("fixture.owner", (), activate, resources=(resource,)),))
    host.activate(FastAPI(), {"fixture.owner": {resource: Source()}})
    method = retained[0].capture
    scopes = method()
    assert len(scopes) == 1
    scopes[0].check()
    assert current_lifetimes() == ()
    host.close()
    with pytest.raises(ValueError, match="closed|not active"):
        method()
    with pytest.raises(ValueError, match="closed|not active"):
        scopes[0].check()


def test_ordered_activation_declared_dependencies_and_reverse_idempotent_shutdown():
    events = []
    capability = Capability("fixture.read", "fixture.base", str)

    def activate_base(context):
        events.append("start base")
        return Activation(
            exports={capability: "capability"}, close=lambda: events.append("stop base")
        )

    def activate_child(context):
        assert context.require(capability) == "capability"
        with pytest.raises(ValueError, match="Undeclared"):
            context.require(Capability("fixture.unknown", "fixture.base", str))
        events.append("start child")
        return Activation(close=lambda: events.append("stop child"))

    host = PluginHost(
        (
            Plugin("fixture.child", ("fixture.base",), activate_child, consumes=(capability,)),
            Plugin("fixture.base", (), activate_base, provides=(capability,)),
        )
    )
    host.activate(FastAPI(), {})
    with pytest.raises(ValueError, match="twice"):
        host.activate(FastAPI(), {})
    host.close()
    host.close()
    assert events == ["start base", "start child", "stop child", "stop base"]
    with pytest.raises(ValueError, match="not active"):
        host.resolve(capability)


def test_failed_activation_releases_started_plugins_without_publishing_routes():
    events = []
    routes = APIRouter()
    routes.add_api_route("/fixture", lambda: {"ready": True})

    def fail(context):
        raise ValueError("Activation failed")

    host = PluginHost(
        (
            Plugin(
                "fixture.base",
                (),
                lambda ctx: Activation(routers=(routes,), close=lambda: events.append("released")),
            ),
            Plugin("fixture.failing", ("fixture.base",), fail),
        )
    )
    app = FastAPI()
    before = list(app.routes)
    with pytest.raises(ValueError, match="Activation failed"):
        host.activate(app, {})
    assert events == ["released"]
    assert app.routes == before


def test_fixture_plugin_contributes_http_and_diagnostics_without_host_dispatch_changes(tmp_path):
    def activate(context):
        from asterion.platform.diagnostics import ServiceState

        route = APIRouter()
        route.add_api_route("/api/v1/fixture", lambda: {"plugin": context.plugin.id})
        return Activation(
            routers=(route,),
            hooks={
                "services": (
                    lambda ready: [
                        ServiceState(id="fixture", name="Fixture", state="ready", detail="offline")
                    ],
                )
            },
        )

    settings = Settings(token="fixture-plugin-host-token", data_root=tmp_path)
    engine = create_engine(f"sqlite:///{tmp_path}/plugins.db")
    plugins = (*builtin_plugins(), Plugin("fixture.report", (), activate))
    with TestClient(
        create_app(settings, raw_engine(engine), plugins=plugins),
        headers={"Authorization": f"Bearer {settings.token}"},
    ) as client:
        assert client.get("/api/v1/fixture").json() == {"plugin": "fixture.report"}
        assert any(
            item["id"] == "fixture" for item in client.get("/api/v1/services").json()["services"]
        )
    engine.dispose()


def test_omitted_feature_has_no_routes_and_required_dependents_fail(tmp_path):
    identity = next(plugin for plugin in builtin_plugins() if plugin.id == "asterion.identity")
    data = next(plugin for plugin in builtin_plugins() if plugin.id == "asterion.data")
    with pytest.raises(ValueError, match="Missing required"):
        PluginHost((data,))
    engine = create_engine(f"sqlite:///{tmp_path}/minimal.db")
    settings = Settings(token="minimal-host-test-token-long-enough", data_root=tmp_path)
    with TestClient(
        create_app(settings, raw_engine(engine), plugins=(identity,)),
        headers={"Authorization": f"Bearer {settings.token}"},
    ) as client:
        assert client.get("/api/v1/health").status_code == 200
        assert client.get("/api/v1/data/providers").status_code == 404
        assert client.get("/api/v1/research/runs").status_code == 404
        assert client.get("/api/v1/services").status_code == 200
    engine.dispose()


@pytest.mark.parametrize("invalid", ["owner", "duplicate", "dependency", "missing", "type"])
def test_capability_declarations_fail_before_activation(invalid):
    calls = []
    capability = Capability("fixture.read", "fixture.base", str)
    base = Plugin("fixture.base", (), lambda ctx: calls.append(ctx), provides=(capability,))
    child = Plugin(
        "fixture.child", (base.id,), lambda ctx: calls.append(ctx), consumes=(capability,)
    )
    if invalid == "owner":
        base = replace(base, provides=(replace(capability, provider=child.id),))
    elif invalid == "duplicate":
        base = replace(base, provides=(capability, capability))
    elif invalid == "dependency":
        child = replace(child, requires=())
    elif invalid == "missing":
        base = replace(base, provides=())
    else:
        child = replace(child, consumes=(replace(capability, contract=int),))
    with pytest.raises(ValueError):
        PluginHost((base, child))
    assert calls == []


@pytest.mark.parametrize("exports", ["missing", "undeclared", "wrong_type", "invalid_key"])
def test_invalid_exports_release_resources_and_publish_no_routes(exports):
    capability = Capability("fixture.read", "fixture.base", str)
    values = {
        "missing": {},
        "undeclared": {capability: "ok", Capability("fixture.write", "fixture.base", str): "bad"},
        "wrong_type": {capability: 1},
        "invalid_key": {"invalid": "not a capability"},
    }
    events = []
    app = FastAPI()
    routes = APIRouter()
    routes.add_api_route("/fixture", lambda: {"ready": True})
    before = list(app.routes)
    host = PluginHost(
        (
            Plugin(
                "fixture.base",
                (),
                lambda ctx: Activation(
                    exports=values[exports],
                    routers=(routes,),
                    close=lambda: events.append("closed"),
                ),
                provides=(capability,),
            ),
        )
    )
    with pytest.raises((ValueError, TypeError)):
        host.activate(app, {})
    assert events == ["closed"]
    assert app.routes == before
    with pytest.raises(ValueError, match="not active"):
        host.resolve(capability)


def test_hook_observation_must_be_declared():
    def activate(context):
        assert not hasattr(context, "host")
        with pytest.raises(ValueError, match="Undeclared hook"):
            context.hooks("fixture.private")
        assert context.hooks("fixture.events") == ()
        return Activation()

    host = PluginHost((Plugin("fixture.base", (), activate, observes=("fixture.events",)),))
    host.activate(FastAPI(), {})
    host.close()


@pytest.mark.parametrize("invalid", ["missing", "extra", "type", "duplicate", "unknown_plugin"])
def test_resource_grants_fail_before_any_activation(invalid):
    calls = []
    resource = Resource("fixture.config", str)
    plugin = Plugin("fixture.base", (), lambda ctx: calls.append(ctx), resources=(resource,))
    values = {resource: "approved"}
    bindings = {plugin.id: values}
    if invalid == "missing":
        values.clear()
    elif invalid == "extra":
        values[Resource("fixture.other", str)] = "not requested"
    elif invalid == "type":
        values[resource] = 3
    elif invalid == "duplicate":
        plugin = replace(plugin, resources=(resource, resource))
    else:
        bindings["fixture.unknown"] = {}
    host = PluginHost((plugin,))
    with pytest.raises((ValueError, TypeError)):
        host.activate(FastAPI(), bindings)
    assert calls == []


def test_plugin_only_resolves_declared_resources_and_bindings_are_snapshotted():
    resource = Resource("fixture.config", str)
    private = Resource("fixture.private", str)
    contexts = []
    values = {resource: "approved"}

    def activate(context):
        contexts.append(context)
        values[resource] = "changed"
        assert context.resource(resource) == "approved"
        with pytest.raises(ValueError, match="Undeclared resource"):
            context.resource(private)
        assert set(context.__dataclass_fields__) == {
            "plugin",
            "_resources",
            "_resolve",
            "_hooks",
            "_publisher",
        }
        return Activation()

    host = PluginHost((Plugin("fixture.base", (), activate, resources=(resource,)),))
    host.activate(FastAPI(), {"fixture.base": values})
    host.close()
    with pytest.raises(ValueError, match="closed"):
        contexts[0].resource(resource)


def test_duplicate_exception_handlers_release_plugins_without_publishing():
    events = []

    class Failure(Exception):
        pass

    def activate(context):
        return Activation(
            exception_handlers=((Failure, lambda request, error: None),),
            close=lambda: events.append(context.plugin.id),
        )

    host = PluginHost((Plugin("fixture.one", (), activate), Plugin("fixture.two", (), activate)))
    app = FastAPI()
    before = dict(app.exception_handlers)
    with pytest.raises(ValueError, match="Duplicate exception handler"):
        host.activate(app, {})
    assert app.exception_handlers == before
    assert events == ["fixture.two", "fixture.one"]


def test_zero_plugin_host_and_reserved_kernel_namespace():
    host = PluginHost(())
    host.activate(FastAPI(), {})
    host.close()
    with pytest.raises(ValueError, match="Reserved kernel"):
        PluginHost((Plugin("kernel.authorization", (), lambda _: Activation()),))
    with pytest.raises(ValueError, match="must belong to L3"):
        PluginHost((Plugin("fixture.python", (), lambda _: Activation(), layer="L2"),))


def test_previously_resolved_callback_is_invalid_after_host_close():
    from collections.abc import Callable
    from dataclasses import dataclass

    @dataclass(frozen=True)
    class Port:
        read: Callable[[], str]

    capability = Capability("fixture.read", "fixture.base", Port)
    host = PluginHost(
        (
            Plugin(
                "fixture.base",
                (),
                lambda _: Activation(exports={capability: Port(lambda: "active")}),
                provides=(capability,),
            ),
        )
    )
    host.activate(FastAPI(), {})
    port = host.resolve(capability)
    assert port.read() == "active"
    host.close()
    with pytest.raises(ValueError, match="closed"):
        port.read()


def test_standalone_catalogue_owns_its_host_until_explicit_close():
    from asterion.distribution import strategy_catalog

    with strategy_catalog() as catalog:
        strategy = catalog.resolve(catalog.list()[0].identity)
        parameters = {field.key: field.default for field in strategy.info.parameters}
        strategy.parameters(parameters)
    with pytest.raises(ValueError, match="closed"):
        strategy.parameters(parameters)
    with pytest.raises(ValueError, match="not active"):
        catalog.list()
