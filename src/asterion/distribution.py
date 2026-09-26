from contextlib import ExitStack, contextmanager

from asterion_bindings.execution import ExecutionFactory

from asterion.connections.plugin import plugin as connections
from asterion.connector_ctp.plugin import plugin as ctp
from asterion.research.execution import EXECUTION

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
    """Own the strategy host for exactly the returned catalogue's lifetime."""
    from weakref import finalize

    from asterion_bindings.plugin_host import PluginHost
    from fastapi import FastAPI

    from asterion.extensions.public import PACKAGES
    from asterion.research.strategies import STRATEGIES, StrategyCatalog

    class HostedCatalog(StrategyCatalog):
        # Product ownership only: each operation delegates to the approved
        # strategy catalogue. No strategy lookup or execution is duplicated.
        def __init__(self, host):
            self._host = host
            self._release = finalize(self, host.close)

        def list(self):
            return self._host.resolve(STRATEGIES).list()

        def resolve(self, identity):
            return self._host.resolve(STRATEGIES).resolve(identity)

        def close(self):
            self._release()

        def __enter__(self):
            return self

        def __exit__(self, *exception):
            self.close()

    host = PluginHost(strategy_plugins(installed=root is not None))
    host.activate(
        FastAPI(),
        {"asterion.strategy_catalog": {PACKAGES: extension_packages(root)}}
        if root is not None
        else {},
    )
    return HostedCatalog(host)


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


def granted_data_root(settings):
    """A granted directory exists before any plugin opens a handle below it."""
    settings.data_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    return settings.data_root


def bootstrap_resources(settings, engine, tasks, plugins, owners: ExitStack):
    from asterion_bindings.data_sources import SourceCredentials
    from asterion_bindings.secrets import secret_port
    from asterion_bindings.task_repository import task_port

    from asterion.connections.public import CONNECTOR_OWNERS
    from asterion.connections.public import CREDENTIAL_SCOPE as CONNECTION_SCOPE
    from asterion.connections.public import CREDENTIALS as CONNECTION_CREDENTIALS
    from asterion.data.public import CREDENTIALS
    from asterion.distribution_storage import (
        data_storage,
        research_storage,
        role_storage,
        rule_storage,
    )
    from asterion.extensions.public import PACKAGES
    from asterion.identity.public import ACCOUNT_POLICY, AccountPolicy
    from asterion.platform.resources import DATA_ROOT, STORAGE, TASKS

    data_root = granted_data_root(settings)
    policies = {
        "asterion.strategy_catalog": {PACKAGES: extension_packages(settings.data_root)},
        contract_rules.id: {STORAGE: rule_storage(engine)},
        contract_roles.id: {
            STORAGE: role_storage(engine),
            TASKS: task_port(tasks, frozenset(handler.kind for handler in contract_roles.handlers)),
        },
        extensions.id: {PACKAGES: extension_packages(settings.data_root)},
        identity.id: {ACCOUNT_POLICY: AccountPolicy(settings.require_account)},
        data.id: {
            STORAGE: data_storage(engine),
            DATA_ROOT: data_root,
            CREDENTIALS: SourceCredentials(settings.token, data_root),
            # Sync tasks are executed by the entry; the data owner still queues them.
            TASKS: task_port(tasks, frozenset({"data.sync"})),
        },
        market.id: {DATA_ROOT: data_root},
        connections.id: {
            DATA_ROOT: data_root,
            CONNECTION_CREDENTIALS: secret_port(settings.token, CONNECTION_SCOPE),
            CONNECTOR_OWNERS: {"ctp": ctp.id},
        },
        research.id: {
            EXECUTION: owners.enter_context(ExecutionFactory()),
            STORAGE: research_storage(engine),
            TASKS: task_port(tasks, frozenset(handler.kind for handler in research.handlers)),
        },
    }
    return {plugin.id: policies.get(plugin.id, {}) for plugin in plugins}


@contextmanager
def execution_resources(settings, kind, post):
    """Approved task-specific bindings, constructed inside the trusted worker bootstrap."""
    from asterion.research.strategies import STRATEGY_RESOURCE

    with ExitStack() as owners:
        factories = {
            "research.backtest": lambda: {
                STRATEGY_RESOURCE: owners.enter_context(strategy_catalog(settings.data_root)),
                EXECUTION: owners.enter_context(ExecutionFactory()),
            },
        }
        factory = factories.get(kind)
        yield factory() if factory else {}


@contextmanager
def backup_inputs(conn, state, token, plugins):
    """Read-only evidence assembly; validators never receive the connection or runtime key."""
    from asterion_bindings.artifacts import ArtifactStore
    from asterion_bindings.data_sources import SourceCredentials
    from asterion_bindings.files import read_files

    from asterion.contract_rules.backup import load_evidence as rule_evidence
    from asterion.data.backup import load_evidence as data_evidence
    from asterion.identity.backup import load_evidence as identity_evidence
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
    from asterion.trading_time.plugin import load_evidence as time_evidence

    factories = {
        contract_roles.id: lambda: RoleBackup(
            tuple(dict(r) for r in conn.execute(select(role_versions)).mappings()),
            snapshot_backup_access(conn, ArtifactStore(state / "data", read_only=True)),
            tuple(dict(r) for r in conn.execute(select(computed_versions)).mappings()),
            tuple(dict(r) for r in conn.execute(select(workflows)).mappings()),
        ),
        trading_time.id: lambda: time_evidence(conn),
        identity.id: lambda: identity_evidence(conn),
        contract_rules.id: lambda: rule_evidence(conn),
        data.id: lambda: data_evidence(
            conn,
            ArtifactStore(state / "data", read_only=True),
            read_files(state / "data"),
            SourceCredentials(token, state / "data").opens,
        ),
        research.id: lambda: research_evidence(
            conn, strategy_catalog(), owners.enter_context(ExecutionFactory()), validate_external
        ),
    }
    with ExitStack() as owners:
        yield {plugin.id: factories[plugin.id]() for plugin in plugins if plugin.backup is not None}


def restore_inputs(conn, plugins, scope):
    """Explicit approval of domain restore operations within the shared transaction."""
    from asterion.identity.backup import restore_inputs as identity_inputs

    factories = {identity.id: lambda: identity_inputs(conn, scope)}
    return {plugin.id: factories[plugin.id]() for plugin in plugins if plugin.restore is not None}


def extension_packages(root):
    from asterion.platform.extensions.packages import Packages
    from asterion.research.external import validate_contribution as validate_strategy

    return Packages(root / ".extensions", {"research.strategy": validate_strategy})


def request_policies():
    """Approved product capabilities. Server grants are authoritative."""
    from asterion_bindings.authority import Grant

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
        "tasks": (
            read("/data/jobs", True),
            Grant("/data/jobs/:id/retry", ("POST",)),
            Grant("/contract-roles/computed/tasks/:id/retry", ("POST",)),
        ),
        "roles": (
            read("/contract-roles/hierarchy"),
            read("/contract-roles"),
            read("/contract-roles/computed"),
            read("/contract-roles/computed/:id/sync-plan"),
            read("/contract-roles/computed/:id/sync-workflows"),
            read("/contract-roles/computed/sync-workflows/:id"),
            read("/contract-roles/computed/tasks/:id"),
            Grant("/contract-roles/computed/sync-batches", ("POST",)),
            Grant("/contract-roles/candidates/preview", ("POST",)),
            Grant("/contract-roles/computed/preview", ("POST",)),
            Grant("/contract-roles/computed/start", ("POST",)),
            read("/data/providers"),
            read("/data/catalog"),
            read("/trading-time"),
        ),
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
