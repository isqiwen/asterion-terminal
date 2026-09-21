"""Connection-owned profiles, secrets and a single active source lifetime."""

import json
import os
import threading
from typing import Literal
from uuid import uuid4

from pydantic import Field, SecretStr, model_validator

from .public import (
    AccountBatch,
    ChannelState,
    ConnectionProfile,
    ConnectorContribution,
    ConnectorDescriptor,
    ConnectorError,
    InstrumentBatch,
    SourceModel,
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


class SavedConnection(ConnectionProfile):
    encrypted: str | None


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


def atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as file:
        file.write(content)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)


class Runtime:
    def __init__(self):
        self.session = None
        self.channels = {c: ChannelState() for c in ("market", "account")}
        self.cancel = threading.Event()
        self.subscriptions = []


class ConnectionService:
    def __init__(self, root, secrets, owners):
        self.path = root / "connections" / "profiles.json"
        self.secrets, self.owners = secrets, owners
        self.lock, self.operation = threading.RLock(), threading.RLock()
        self.catalog, self.saved, self.runtime, self.listeners = {}, {}, {}, []
        self.selected_id = None
        self.initialized = False
        self.closed = False
        self.notice = ""
        self.load_error = False
        if (root / "market" / "simnow.json").exists():
            self.notice = "检测到不支持的旧连接配置，原文件已保留；请在此重新配置连接。"
        try:
            if self.path.exists():
                data = json.loads(self.path.read_text())
                if set(data) != {"version", "connections", "selected_id"} or data["version"] != 2:
                    raise ValueError()
                for value in data["connections"]:
                    row = SavedConnection.model_validate(value)
                    if row.connection_id in self.saved:
                        raise ValueError()
                    self.saved[row.connection_id] = row
                    self._secrets(row)
                if data["selected_id"] is not None and data["selected_id"] not in self.saved:
                    raise ValueError()
                self.selected_id = data["selected_id"]
        except Exception:  # noqa: BLE001 - never disclose encryption input
            self.saved = {}
            self.load_error = True
            self.notice = "连接配置校验失败，原文件已保留；当前配置不受支持。"

    def initialize(self, factories):
        with self.lock:
            if self.initialized:
                raise ValueError("接入目录已经初始化")
            catalog = {}
            for factory in factories:
                item = factory()
                if not isinstance(item, ConnectorContribution):
                    raise TypeError("接入贡献类型无效")
                descriptor = ConnectorDescriptor.model_validate(item.descriptor.model_dump())
                if self.owners.get(descriptor.id) != descriptor.owner or descriptor.id in catalog:
                    raise ValueError("接入贡献归属错误或 ID 重复")
                if not callable(item.validate) or not callable(item.factory):
                    raise TypeError("接入工厂无效")
                catalog[descriptor.id] = item
            self.catalog = catalog
            self.initialized = True

    def _secrets(self, row):
        if row.encrypted is None:
            return {}
        content = json.loads(self.secrets.decrypt(row.encrypted.encode()))
        profile = row.model_dump(exclude={"encrypted"})
        if set(content) != {"profile", "secrets"} or content["profile"] != profile:
            raise ValueError("凭据与连接身份不匹配")
        values = content["secrets"]
        if not isinstance(values, dict) or any(
            not isinstance(k, str) or not isinstance(v, str) for k, v in values.items()
        ):
            raise ValueError("秘密格式无效")
        return values

    def profile(self, connection_id):
        with self.lock:
            if connection_id not in self.saved:
                raise ValueError("连接不存在，请先配置连接")
            return ConnectionProfile.model_validate(
                self.saved[connection_id].model_dump(exclude={"encrypted"})
            )

    def _runtime(self, connection_id):
        self.profile(connection_id)
        return self.runtime.setdefault(connection_id, Runtime())

    def _connector(self, connection_id, feature=None):
        if self.closed or not self.initialized:
            raise ConnectorError("连接服务尚未就绪", "unavailable", False)
        profile = self.profile(connection_id)
        contribution = self.catalog.get(profile.connector_id)
        if contribution is None:
            raise ConnectorError("所需接入插件不可用，配置已保留", "unsupported", False)
        if feature and feature not in contribution.descriptor.capabilities:
            raise ConnectorError("当前接入不支持此功能", "unsupported", False)
        return contribution

    def supports(self, connection_id, feature):
        with self.lock:
            try:
                self._connector(connection_id, feature)
                return True
            except ValueError:
                return False

    def profiles(self):
        with self.lock:
            return list(self.saved)

    def channel(self, connection_id, channel):
        with self.lock:
            return self._runtime(connection_id).channels[channel].model_copy()

    def view(self, connection_id):
        with self.lock:
            profile = self.profile(connection_id)
            runtime = self._runtime(connection_id)
            descriptor = self.catalog.get(profile.connector_id)
            secrets = self._secrets(self.saved[connection_id])
            return ConnectionView(
                **profile.model_dump(),
                secret_saved={k: bool(v) for k, v in secrets.items()},
                capabilities=descriptor.descriptor.capabilities if descriptor else [],
                available=descriptor is not None,
                market=runtime.channels["market"].model_copy(),
                account=runtime.channels["account"].model_copy(),
            )

    def snapshot(self):
        with self.operation, self.lock:
            return ConnectionList(
                connections=[self.view(k) for k in self.saved],
                connectors=[v.descriptor for v in self.catalog.values()],
                selected_id=self.selected_id,
                active_id=self.active_id(),
                notice=self.notice,
            )

    def save(self, body):
        with self.operation, self.lock:
            if self.load_error:
                raise ValueError(self.notice)
            if not body.name.strip():
                raise ValueError("连接名称不能为空")
            item = self.catalog.get(body.connector_id)
            if item is None:
                raise ValueError("接入方式不受支持")
            fields = {f.key: f for f in item.descriptor.fields}
            if set(body.config) != {k for k, f in fields.items() if not f.secret} or set(
                body.secrets
            ) != {k for k, f in fields.items() if f.secret}:
                raise ValueError("配置字段不符合接入契约")
            existing = self.saved.get(body.connection_id)
            if body.connection_id is not None and existing is None:
                raise ValueError("连接不存在")
            if existing:
                if body.expected_revision != existing.config_revision:
                    raise ValueError("配置已改变，请重新加载")
                if body.connector_id != existing.connector_id or any(
                    f.identity and body.config[k] != existing.config[k] for k, f in fields.items()
                ):
                    raise ValueError("账户身份改变请保存为另一份配置")
                if self._runtime(existing.connection_id).session is not None or any(
                    c.state != "disconnected"
                    for c in self._runtime(existing.connection_id).channels.values()
                ):
                    raise ValueError("请先断开连接再修改配置")
            values = self._secrets(existing) if existing else {}
            if (
                existing
                and body.config != existing.config
                and any(v.action == "keep" for v in body.secrets.values())
            ):
                raise ValueError("连接目标改变，请重新输入凭据确认")
            for key, change in body.secrets.items():
                if change.action == "replace":
                    values[key] = change.value.get_secret_value()
                elif change.action == "clear":
                    values.pop(key, None)
            for key, field in fields.items():
                if not field.secret and field.required and not body.config[key].strip():
                    raise ValueError("必填配置不能为空")
            if all(values.get(k) for k, f in fields.items() if f.secret and f.required):
                item.validate(body.config, values)
            # Incomplete drafts (including explicit secret clearing) can be saved, never connected.
            profile = ConnectionProfile(
                connection_id=body.connection_id or uuid4().hex,
                connector_id=body.connector_id,
                name=body.name,
                config_revision=existing.config_revision + 1 if existing else 1,
                config=body.config,
            )
            encrypted = (
                self.secrets.encrypt(
                    json.dumps({"profile": profile.model_dump(), "secrets": values}).encode()
                ).decode()
                if values
                else None
            )
            row = SavedConnection(**profile.model_dump(), encrypted=encrypted)
            updated = self.saved | {row.connection_id: row}
            selected = self.selected_id or row.connection_id
            self._persist(updated, selected)
            self.saved, self.selected_id = updated, selected
            return self.view(row.connection_id)

    def delete(self, connection_id, expected_revision):
        with self.operation, self.lock:
            if self.load_error:
                raise ValueError(self.notice)
            profile = self.profile(connection_id)
            if profile.config_revision != expected_revision:
                raise ValueError("配置已改变，请重新加载后删除")
            runtime = self._runtime(connection_id)
            if runtime.session is not None or any(
                channel.state != "disconnected" for channel in runtime.channels.values()
            ):
                raise ValueError("请先断开连接再删除配置")
            updated = {key: row for key, row in self.saved.items() if key != connection_id}
            selected = None if self.selected_id == connection_id else self.selected_id
            # Remove profile and credentials together; business caches/history remain untouched.
            self._persist(updated, selected)
            self.saved, self.selected_id = updated, selected
            runtime.cancel.set()
            self.runtime.pop(connection_id, None)
            return self.snapshot()

    def _persist(self, profiles, selected):
        atomic(
            self.path,
            json.dumps(
                {
                    "version": 2,
                    "selected_id": selected,
                    "connections": [r.model_dump() for r in profiles.values()],
                }
            ),
        )

    def active_id(self):
        with self.lock:
            return next(
                (key for key, runtime in self.runtime.items() if runtime.session is not None), None
            )

    def select(self, connection_id):
        with self.operation:
            self.profile(connection_id)
            if self.active_id() is not None:
                return self.connect(connection_id)
            with self.lock:
                self._persist(self.saved, connection_id)
                self.selected_id = connection_id
            return self.view(connection_id)

    def connect(self, connection_id):
        with self.operation:
            with self.lock:
                item = self._connector(connection_id)
                profile = self.profile(connection_id)
                values = self._secrets(self.saved[connection_id])
                item.validate(profile.config, values)
                if self.active_id() == connection_id:
                    return self.view(connection_id)
            # Close and cancel the old source before constructing any new session.
            for key in self.profiles():
                self.disconnect(key)
            with self.lock:
                self._persist(self.saved, connection_id)
                self.selected_id = connection_id
                runtime = self._runtime(connection_id)
                try:
                    runtime.session = item.factory(profile, values)
                except Exception:  # noqa: BLE001 - sanitize connector failures
                    raise ConnectorError("连接启动失败，请检查参数及运行组件") from None
            try:
                if {"account_snapshot", "instrument_catalog"} & set(item.descriptor.capabilities):
                    self._connect_channel(connection_id, "account")
                if "market_quotes" in item.descriptor.capabilities:
                    self._connect_channel(connection_id, "market")
            except Exception:  # noqa: BLE001 - sanitize connector failures
                self.disconnect(connection_id)
                raise ConnectorError("连接启动失败，请检查参数及运行组件") from None
            return self.view(connection_id)

    def disconnect(self, connection_id):
        with self.operation:
            self._disconnect_channel(connection_id, "account")
            return self._disconnect_channel(connection_id, "market")

    def listen(self, callback):
        with self.lock:
            self.listeners.append(callback)

        def remove():
            with self.lock:
                if callback in self.listeners:
                    self.listeners.remove(callback)

        return remove

    def _event(self, connection_id, generation, kind, value):
        with self.lock:
            if connection_id not in self.saved:
                return
            runtime = self._runtime(connection_id)
            state = runtime.channels["market"]
            if generation != state.generation or state.state == "disconnected":
                return
            mapping = {
                "connected": "ready",
                "connecting": "authenticating",
                "reconnecting": "reconnecting",
                "error": "error",
            }
            if kind in mapping:
                state.state = mapping[kind]
                state.detail = (
                    value
                    or {
                        "connected": "行情已登录",
                        "connecting": "正在登录行情",
                        "reconnecting": "行情重连中",
                        "error": "行情连接失败",
                    }[kind]
                )
            listeners = tuple(self.listeners)
        for callback in listeners:
            callback(connection_id, generation, kind, value)

    def _connect_channel(self, connection_id, channel):
        with self.operation:
            with self.lock:
                runtime = self._runtime(connection_id)
                state = runtime.channels[channel]
                state.state, state.detail = (
                    "connecting",
                    "正在连接" if channel == "market" else "等待账户查询认证",
                )
                generation, session = state.generation, runtime.session
                if channel == "account":
                    runtime.cancel = threading.Event()
                subscriptions = list(runtime.subscriptions)
            if channel == "market":
                try:
                    session.start_market(
                        subscriptions,
                        lambda kind, value: self._event(connection_id, generation, kind, value),
                    )
                except Exception:  # noqa: BLE001 - sanitize connector failures
                    raise ConnectorError("行情接口启动失败，请检查运行组件后重试") from None
            return self.view(connection_id)

    def _disconnect_channel(self, connection_id, channel):
        with self.operation:
            with self.lock:
                runtime = self._runtime(connection_id)
                state = runtime.channels[channel]
                state.generation += 1
                state.state, state.detail = "disconnected", "已断开"
                if channel == "account":
                    runtime.cancel.set()
                session = runtime.session
                release = all(c.state == "disconnected" for c in runtime.channels.values())
            if session:
                if channel == "market":
                    session.stop_market()
                if release:
                    try:
                        session.close()
                    except Exception:  # noqa: BLE001 - never disclose SDK details
                        raise ConnectorError("原连接尚未完成关闭，请重试断开连接") from None
                    with self.lock:
                        runtime.session = None
            return self.view(connection_id)

    def subscribe(self, connection_id, subscriptions):
        with self.operation:
            with self.lock:
                self._connector(connection_id, "market_quotes")
                runtime = self._runtime(connection_id)
                runtime.subscriptions = list(subscriptions)
                session = runtime.session
            if session:
                session.update_subscriptions(subscriptions)

    def read(self, connection_id, kind, cancel):
        feature = "instrument_catalog" if kind == "instruments" else "account_snapshot"
        with self.lock:
            self._connector(connection_id, feature)
            if kind == "account":
                self._connector(connection_id, "positions")
            runtime = self._runtime(connection_id)
            state = runtime.channels["account"]
            if state.state == "disconnected" or runtime.session is None:
                raise ConnectorError("请先连接已保存的配置", "session_invalid", False)
            session, generation, channel_cancel = runtime.session, state.generation, runtime.cancel

        class Cancellation:
            def is_set(self):
                return cancel.is_set() or channel_cancel.is_set()

        try:
            result = getattr(session, kind)(generation, Cancellation())
            batch_type = InstrumentBatch if kind == "instruments" else AccountBatch
            result = batch_type.model_validate(result.model_dump())
            with self.lock:
                if (
                    channel_cancel.is_set()
                    or cancel.is_set()
                    or generation != runtime.channels["account"].generation
                    or result.connection_id != connection_id
                    or result.generation != generation
                ):
                    raise ConnectorError("连接已变更，响应已丢弃", "session_invalid", False)
                runtime.channels["account"].state = "ready"
                runtime.channels["account"].detail = "账户数据可用"
            return result
        except Exception as error:  # noqa: BLE001
            safe = (
                error
                if isinstance(error, ConnectorError)
                else ConnectorError("来源查询失败或响应未通过校验", "incomplete")
            )
            with self.lock:
                if (
                    generation == runtime.channels["account"].generation
                    and not channel_cancel.is_set()
                ):
                    runtime.channels["account"].state = "error"
                    runtime.channels["account"].detail = str(safe)
            raise safe from None

    def instruments(self, connection_id, cancel):
        return self.read(connection_id, "instruments", cancel)

    def account(self, connection_id, cancel):
        return self.read(connection_id, "account", cancel)

    def close(self):
        with self.operation:
            if self.closed:
                return
            for connection_id in self.profiles():
                self.disconnect(connection_id)
            self.closed = True
