from asterion.connections.plugin import plugin as connections
from asterion.connector_ctp.plugin import plugin as ctp

"""Default distribution manifest. Only this assembly layer selects product plugins."""

from asterion.contract_roles.plugin import plugin as contract_roles
from asterion.contract_rules.plugin import plugin as contract_rules
from asterion.data.plugin import plugin as data
from asterion.extensions.plugin import plugin as extensions
from asterion.identity.plugin import plugin as identity
from asterion.market.plugin import plugin as market
from asterion.research.plugin import plugin as research
from asterion.trading.plugin import plugin as trading
from asterion.trading_time.plugin import plugin as trading_time


def strategy_plugins(*, installed=False):
    from asterion.research.strategies import catalog_plugin
    from asterion.strategies import momentum, sma

    return (
        sma.plugin,
        momentum.plugin,
        catalog_plugin((sma.CAPABILITY, momentum.CAPABILITY), installed=installed),
    )


def strategy_catalog(root=None):
    from fastapi import FastAPI

    from asterion.extensions.public import PACKAGES
    from asterion.platform.plugins import PluginHost
    from asterion.research.strategies import STRATEGIES

    host = PluginHost(strategy_plugins(installed=root is not None))
    host.activate(
        FastAPI(),
        {"asterion.strategy_catalog": {PACKAGES: extension_packages(root)}}
        if root is not None
        else {},
    )
    try:
        return host.resolve(STRATEGIES)
    finally:
        host.close()


def builtin_plugins():
    return (
        *strategy_plugins(installed=True),
        identity,
        trading_time,
        extensions,
        data,
        contract_rules,
        contract_roles,
        research,
        connections,
        ctp,
        market,
        trading,
    )


def bootstrap_resources(settings, engine, tasks, plugins):
    from asterion.connections.public import CONNECTOR_OWNERS
    from asterion.connections.public import CREDENTIAL_SCOPE as CONNECTION_SCOPE
    from asterion.connections.public import CREDENTIALS as CONNECTION_CREDENTIALS
    from asterion.data.public import CREDENTIAL_SCOPE, CREDENTIALS
    from asterion.distribution_storage import (
        data_storage,
        identity_storage,
        research_storage,
        role_storage,
        rule_storage,
        time_storage,
    )
    from asterion.extensions.public import PACKAGES
    from asterion.identity.public import ACCOUNT_POLICY, IDENTITY_DIGEST, AccountPolicy
    from asterion.platform.resources import DATA_ROOT, STORAGE, TASKS
    from asterion.platform.secrets import digest_port, secret_port
    from asterion.platform.task_port import task_port

    policies = {
        "asterion.strategy_catalog": {PACKAGES: extension_packages(settings.data_root)},
        contract_rules.id: {STORAGE: rule_storage(engine)},
        contract_roles.id: {
            STORAGE: role_storage(engine),
            TASKS: task_port(tasks, frozenset(handler.kind for handler in contract_roles.handlers)),
        },
        trading_time.id: {STORAGE: time_storage(engine)},
        extensions.id: {PACKAGES: extension_packages(settings.data_root)},
        identity.id: {
            STORAGE: identity_storage(engine),
            DATA_ROOT: settings.data_root,
            IDENTITY_DIGEST: digest_port(settings.token),
            ACCOUNT_POLICY: AccountPolicy(settings.require_account, settings.account_verification),
        },
        data.id: {
            STORAGE: data_storage(engine),
            DATA_ROOT: settings.data_root,
            CREDENTIALS: secret_port(settings.token, CREDENTIAL_SCOPE),
            TASKS: task_port(tasks, frozenset(handler.kind for handler in data.handlers)),
        },
        market.id: {DATA_ROOT: settings.data_root},
        connections.id: {
            DATA_ROOT: settings.data_root,
            CONNECTION_CREDENTIALS: secret_port(settings.token, CONNECTION_SCOPE),
            CONNECTOR_OWNERS: {"ctp": ctp.id},
        },
        research.id: {
            STORAGE: research_storage(engine),
            TASKS: task_port(tasks, frozenset(handler.kind for handler in research.handlers)),
        },
    }
    return {plugin.id: policies.get(plugin.id, {}) for plugin in plugins}


def execution_resources(settings, kind, post):
    """Approved task-specific bindings, constructed inside the trusted worker bootstrap."""
    from asterion.data.public import CREDENTIAL_SCOPE, CREDENTIALS, SYNC_REPORTER, SyncReporter
    from asterion.platform.resources import DATA_ROOT
    from asterion.platform.secrets import secret_port

    def sync_resources():
        def progress(completed, total):
            post("/progress", {"completed": completed, "total": total}, lease_in_body=True)

        def checkpoint(index, evidence):
            if type(index) is not int or index < 0:
                raise ValueError("Invalid observation index")
            post(f"/observations/{index}", evidence)

        return {
            DATA_ROOT: settings.data_root,
            CREDENTIALS: secret_port(settings.token, CREDENTIAL_SCOPE),
            SYNC_REPORTER: SyncReporter(progress, checkpoint, lambda: post("/resume")),
        }

    from asterion.research.strategies import STRATEGY_RESOURCE

    factories = {
        "data.sync": sync_resources,
        "research.backtest": lambda: {STRATEGY_RESOURCE: strategy_catalog(settings.data_root)},
    }
    factory = factories.get(kind)
    return factory() if factory else {}


def backup_inputs(conn, state, token, plugins):
    """Read-only evidence assembly; validators never receive the connection or runtime key."""
    from asterion.contract_rules.backup import load_evidence as rule_evidence
    from asterion.data.backup import load_evidence as data_evidence
    from asterion.data.public import CREDENTIAL_SCOPE
    from asterion.identity.backup import load_evidence as identity_evidence
    from asterion.platform.files import read_files
    from asterion.platform.secrets import secret_port
    from asterion.research.backup import load_evidence as research_evidence
    from asterion.research.external import artifact
    from asterion.research.parameters import validate_parameters
    from asterion.research.strategies import StrategyRef

    def validate_external(request):
        info, _ = artifact(
            extension_packages(state / "data"), StrategyRef.model_validate(request["strategy"])
        )
        validate_parameters(info.parameters, request["parameters"])

    from sqlalchemy import select

    from asterion.contract_roles.plugin import RoleBackup, computed_versions
    from asterion.contract_roles.plugin import versions as role_versions
    from asterion.contract_roles.sync_workflow import workflows
    from asterion.data.public import snapshot_backup_access
    from asterion.trading_time.plugin import versions as time_versions

    factories = {
        contract_roles.id: lambda: RoleBackup(
            tuple(dict(r) for r in conn.execute(select(role_versions)).mappings()),
            snapshot_backup_access(conn, read_files(state / "data")),
            tuple(dict(r) for r in conn.execute(select(computed_versions)).mappings()),
            tuple(dict(r) for r in conn.execute(select(workflows)).mappings()),
        ),
        trading_time.id: lambda: [dict(r) for r in conn.execute(select(time_versions)).mappings()],
        identity.id: lambda: identity_evidence(conn),
        contract_rules.id: lambda: rule_evidence(conn),
        data.id: lambda: data_evidence(
            conn, read_files(state / "data"), secret_port(token, CREDENTIAL_SCOPE).decrypt
        ),
        research.id: lambda: research_evidence(conn, strategy_catalog(), validate_external),
    }
    return {plugin.id: factories[plugin.id]() for plugin in plugins if plugin.backup is not None}


def restore_inputs(conn, plugins, scope):
    """Explicit approval of domain restore operations within the shared transaction."""
    from asterion.identity.backup import restore_inputs as identity_inputs

    factories = {identity.id: lambda: identity_inputs(conn, scope)}
    return {plugin.id: factories[plugin.id]() for plugin in plugins if plugin.restore is not None}


def extension_packages(root):
    from asterion.data.providers.external import validate_contribution
    from asterion.platform.extensions.packages import Packages
    from asterion.platform.extensions.views import validate_view
    from asterion.research.external import validate_contribution as validate_strategy

    return Packages(
        root / ".extensions",
        {
            "data.provider": validate_contribution,
            "ui.table": validate_view,
            "research.strategy": validate_strategy,
        },
    )


def request_policies():
    """Approved product capabilities. Server grants are authoritative."""
    from asterion.platform.authorization import Grant

    def read(path, descendants=False):
        return Grant(path, ("GET",), descendants)

    def use(path, descendants=True):
        return Grant(path, ("GET", "POST"), descendants)

    return {
        "data": (use("/data"), use("/reference"), use("/imports"), use("/trading-time")),
        "research": (
            read("/reference/releases"),
            read("/reference/releases/:id"),
            use("/research"),
            use("/contract-rules"),
            use("/trading-time"),
            read("/data/catalog", True),
            read("/data/versions/:id"),
            Grant("/data/versions/:id/coverage", ("POST",)),
            read("/data/coverage/:id"),
            read("/data/coverage/:id/refill-status"),
            Grant("/data/coverage/:id/refill", ("POST",)),
            read("/data/preparations"),
            Grant("/data/jobs/:id/retry", ("POST",)),
        ),
        "tasks": (read("/data/jobs", True), Grant("/data/jobs/:id/retry", ("POST",))),
        "market": (
            use("/market"),
            read("/connections"),
            Grant("/connections/:id/select", ("POST",)),
            Grant("/connections/:id/connect", ("POST",)),
            Grant("/connections/:id/disconnect", ("POST",)),
        ),
        "connections": (use("/connections"),),
        "trading": (
            use("/trading"),
            read("/connections"),
        ),
        "sources": (use("/data/connections"), use("/data/providers")),
        "extensions": (use("/extensions"),),
    }
