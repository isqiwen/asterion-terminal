"""L3 HTTP models and source-object adapters for the Rust L2 connection owner."""

import inspect
import json
import threading
import weakref
from typing import Literal

from asterion_bindings import _native
from pydantic import Field, SecretStr, model_validator

from .public import (
    AccountBatch,
    ChannelState,
    ConnectionProfile,
    ConnectorContribution,
    ConnectorDescriptor,
    ConnectorError,
    InstrumentBatch,
    ReadRequest,
    SourceModel,
    Subscription,
)


class SecretChange(SourceModel):
    action: Literal["keep", "replace", "clear"]
    value: SecretStr | None = None

    @model_validator(mode="after")
    def valid(self):
        if (self.action == "replace") != (self.value is not None):
            raise ValueError("替换凭据必须提供值，其他操作不得携带值")
        return self


class SaveConnection(SourceModel):
    connection_id: str | None = Field(default=None, pattern=r"^[a-f0-9]{32}$")
    expected_revision: int | None = Field(default=None, ge=1)
    connector_id: str
    name: str = Field(min_length=1, max_length=60)
    config: dict[str, str]
    secrets: dict[str, SecretChange]


class DeleteConnection(SourceModel):
    expected_revision: int = Field(ge=1)


class ConnectionView(ConnectionProfile):
    secret_saved: dict[str, bool]
    capabilities: list[str]
    available: bool
    market: ChannelState
    account: ChannelState


class ConnectionList(SourceModel):
    connections: list[ConnectionView]
    connectors: list[ConnectorDescriptor]
    selected_id: str | None
    active_id: str | None
    notice: str = ""


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def synchronous(value):
    if inspect.isawaitable(value):
        if inspect.iscoroutine(value):
            value.close()
        raise ValueError("接入回调必须同步完成")
    return value


class _Session:
    """Convert domain DTOs; all authorization and lifecycle order remain native."""

    def __init__(self, session):
        self.session = synchronous(session)
        for method in (
            "start_market",
            "update_subscriptions",
            "stop_market",
            "instruments",
            "account",
            "close",
        ):
            if not callable(getattr(session, method, None)):
                raise TypeError("接入会话契约不完整")

    def start_market(self, subscriptions, emit):
        return self.session.start_market(
            [Subscription.model_validate(v) for v in json.loads(subscriptions)], emit
        )

    def subscriptions(self, subscriptions):
        return self.session.update_subscriptions(
            [Subscription.model_validate(v) for v in json.loads(subscriptions)]
        )

    def stop_market(self):
        return self.session.stop_market()

    def close(self):
        return self.session.close()

    def read(self, kind, request, cancel):
        request = ReadRequest.model_validate_json(request)
        result = synchronous(getattr(self.session, kind)(request, cancel))
        batch_type = InstrumentBatch if kind == "instruments" else AccountBatch
        return batch_type.model_validate(result.model_dump()).model_dump_json()


class _Source:
    def __init__(self, contribution):
        self.contribution = contribution

    def validate(self, config, secrets):
        return self.contribution.validate(json.loads(config), json.loads(secrets))

    def open(self, profile, secrets):
        return _Session(
            self.contribution.factory(
                ConnectionProfile.model_validate_json(profile), json.loads(secrets)
            )
        )


class ConnectionService:
    """Application facade. Owns Python listeners; Rust owns profiles and sessions."""

    def __init__(self, root, secrets, owners):
        self.path = root / "connections" / "profiles.json"
        self._listeners = []
        self._listener_lock = threading.RLock()
        reference = weakref.ref(self)

        def dispatch(connection_id, generation, kind, value):
            service = reference()
            if service is not None:
                with service._listener_lock:
                    callbacks = tuple(service._listeners)
                for callback in callbacks:
                    synchronous(callback(connection_id, generation, kind, value))

        self._native = _native.Connections(
            str(self.path), secrets, encoded(dict(owners.items())), dispatch
        )

    @staticmethod
    def _call(function, *args):
        try:
            return function(*args)
        except _native.ConnectionsError as error:
            raise ConnectorError(str(error), error.category, error.retryable) from None

    def initialize(self, factories):
        items = [synchronous(factory()) for factory in factories]
        for item in items:
            if not isinstance(item, ConnectorContribution):
                raise TypeError("接入贡献类型无效")
            if not callable(item.validate) or not callable(item.factory):
                raise TypeError("接入工厂无效")
        self._call(
            self._native.initialize,
            encoded([item.descriptor.model_dump() for item in items]),
            [_Source(item) for item in items],
        )

    def profiles(self):
        return self._native.profiles()

    def profile(self, connection_id):
        return ConnectionProfile.model_validate_json(
            self._call(self._native.profile, connection_id)
        )

    def supports(self, connection_id, feature):
        return self._call(self._native.supports, connection_id, feature)

    def channel(self, connection_id, channel):
        return ChannelState.model_validate_json(
            self._call(self._native.channel, connection_id, channel)
        )

    def view(self, connection_id):
        return ConnectionView.model_validate_json(self._call(self._native.view, connection_id))

    def snapshot(self):
        return ConnectionList.model_validate_json(self._call(self._native.snapshot))

    def save(self, body):
        value = body.model_dump()
        value["secrets"] = {
            k: {
                "action": v.action,
                "value": v.value.get_secret_value() if v.value is not None else None,
            }
            for k, v in body.secrets.items()
        }
        return ConnectionView.model_validate_json(self._call(self._native.save, encoded(value)))

    def delete(self, connection_id, expected_revision):
        return ConnectionList.model_validate_json(
            self._call(self._native.delete, connection_id, expected_revision)
        )

    def active_id(self):
        return self._native.active_id()

    def select(self, connection_id):
        return ConnectionView.model_validate_json(self._call(self._native.select, connection_id))

    def connect(self, connection_id):
        return ConnectionView.model_validate_json(self._call(self._native.connect, connection_id))

    def disconnect(self, connection_id):
        return ConnectionView.model_validate_json(
            self._call(self._native.disconnect, connection_id)
        )

    def listen(self, callback):
        with self._listener_lock:
            self._listeners.append(callback)

        def remove():
            with self._listener_lock:
                if callback in self._listeners:
                    self._listeners.remove(callback)

        return remove

    def subscribe(self, connection_id, subscriptions):
        self._call(
            self._native.subscribe, connection_id, encoded([v.model_dump() for v in subscriptions])
        )

    def instruments(self, connection_id, cancel):
        return InstrumentBatch.model_validate_json(
            self._call(self._native.read, connection_id, "instruments", cancel)
        )

    def account(self, connection_id, cancel):
        return AccountBatch.model_validate_json(
            self._call(self._native.read, connection_id, "account", cancel)
        )

    def close(self):
        self._call(self._native.close)
