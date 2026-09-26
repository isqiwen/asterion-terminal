"""Only test code: a second source implementation, never included in production."""

import threading
import time
from contextlib import contextmanager
from dataclasses import replace
from weakref import WeakKeyDictionary

from asterion_bindings.secrets import secret_port

from asterion.connections.public import (
    CREDENTIAL_SCOPE,
    AccountBatch,
    AccountSummary,
    ConfigField,
    ConnectionAccess,
    ConnectorContribution,
    ConnectorDescriptor,
    InstrumentBatch,
    SourceInstrument,
)
from asterion.connections.service import ConnectionService, SaveConnection


def instrument(**changes):
    return SourceInstrument.model_validate(
        {
            "exchange": "SHFE",
            "symbol": "au2612",
            "name": "黄金2612",
            "product": "au",
            "delivery_month": "2026-12",
            "listed_on": "2026-01-01",
            "last_trade_on": "2026-12-15",
        }
        | changes
    )


class Session:
    def __init__(self, profile, secrets):
        self.profile = profile
        self.emit = None
        self.closed = False
        self.subscriptions = []
        self.account_reads = 0
        self.rows = [instrument()]
        self.entered, self.release = threading.Event(), None
        self.fail = False

    def start_market(self, subscriptions, emit):
        self.emit = emit
        self.subscriptions = subscriptions
        emit("connected", None)

    def update_subscriptions(self, subscriptions):
        self.subscriptions = list(subscriptions)

    def stop_market(self):
        pass

    def meta(self, request):
        return {
            **request.model_dump(),
            "observed_at": time.time(),
            "complete": True,
        }

    def instruments(self, request, cancel):
        return InstrumentBatch(**self.meta(request), instruments=self.rows)

    def account(self, request, cancel):
        self.account_reads += 1
        self.entered.set()
        if self.release:
            while not self.release.wait(0.01):
                if cancel.is_set():
                    break
        if self.fail:
            raise RuntimeError("private credential must not leak")
        return AccountBatch(
            **self.meta(request),
            source_account_id=self.profile.config["user"],
            account=AccountSummary(
                currency="CNY",
                trading_day="20260922",
                balance=100,
                available=80,
                margin=20,
                position_profit=0,
                close_profit=0,
                commission=0,
            ),
            positions=[],
        )

    def close(self):
        self.closed = True


def contribution(identifier="fixture", features=None):
    def validate(config, secrets):
        if not config["user"] or not secrets.get("password"):
            raise ValueError("凭据未配置")

    return ConnectorContribution(
        ConnectorDescriptor(
            id=identifier,
            owner="test." + identifier,
            title=identifier,
            capabilities=tuple(features)
            if features is not None
            else ("market_quotes", "instrument_catalog", "account_snapshot", "positions"),
            fields=(
                ConfigField(key="user", label="User", identity=True),
                ConfigField(key="endpoint", label="Endpoint"),
                ConfigField(key="password", label="Password", secret=True),
            ),
        ),
        validate,
        Session,
    )


def save_body(**changes):
    return SaveConnection.model_validate(
        {
            "connector_id": "fixture",
            "name": "测试连接",
            "config": {"user": "investor", "endpoint": "server"},
            "secrets": {"password": {"action": "replace", "value": "test-secret"}},
        }
        | changes
    )


_sessions = WeakKeyDictionary()


def source_session(service, identifier):
    """Test connector owns its SDK objects; no production internals are exposed."""
    return _sessions[service].get(identifier)


@contextmanager
def blocked_profiles_path(service):
    """An actual filesystem publication failure, without replacing native I/O."""
    directory = service.path.parent
    retained = directory.with_name(directory.name + "-retained")
    directory.rename(retained)
    directory.write_text("not-a-directory")
    try:
        yield
    finally:
        directory.unlink()
        retained.rename(directory)


def manager(root, contributions=None):
    contributions = contributions or [contribution()]
    service = ConnectionService(
        root,
        secret_port("test-connections-key", CREDENTIAL_SCOPE),
        {c.descriptor.id: c.descriptor.owner for c in contributions},
    )
    sessions = {}
    _sessions[service] = sessions

    def tracked(item):
        def factory(profile, secrets):
            session = item.factory(profile, secrets)
            sessions[profile.connection_id] = session
            return session

        return replace(item, factory=factory)

    tracked_sources = [tracked(c) for c in contributions]
    service.initialize([lambda c=c: c for c in tracked_sources])
    return service


def access(service):
    return ConnectionAccess(
        *(
            getattr(service, key)
            for key in (
                "profiles",
                "profile",
                "channel",
                "subscribe",
                "instruments",
                "account",
                "listen",
                "supports",
            )
        )
    )
