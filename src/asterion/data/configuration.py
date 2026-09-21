"""Versioned, schema-validated local provider settings; secrets never leave safe views."""

import time
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, SecretStr
from sqlalchemy import Column, Float, Integer, String, Table, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.exc import IntegrityError

from asterion.data.providers.public import ConfigurationSpec, ProviderError
from asterion.platform.store import metadata
from asterion.platform.tasks.service import Conflict

configurations = Table(
    "data_provider_configurations",
    metadata,
    Column("provider", String, primary_key=True),
    Column("revision", Integer, nullable=False),
    Column("schema_version", Integer, nullable=False),
    Column("snapshot_ref", String, nullable=False),
)


verification_records = Table(
    "data_connection_verifications",
    metadata,
    Column("provider", String, primary_key=True),
    Column("revision", Integer, nullable=False),
    Column("started_at", Float, nullable=False),
    Column("checked_at", Float, nullable=False),
    Column("status", String, nullable=False),
)


class VerificationState(BaseModel):
    status: Literal["never", "verified", "failed", "stale"] = "never"
    checked_at: float | None = None
    revision: int | None = None
    message: str = "尚未验证已保存配置"


class ConfigurationUpdate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    expected_revision: int = Field(ge=0)
    values: dict[str, Any] = Field(default_factory=dict, max_length=30)
    secrets: dict[str, SecretStr | None] = Field(default_factory=dict, max_length=30)


class ConfigurationState(BaseModel):
    provider: str
    revision: int
    schema_version: int
    values: dict[str, Any]
    secret_fields: list[str]
    configured: bool
    error: str | None = None


class ConfigurationCheck(BaseModel):
    status: str = "verified"
    message: str
    revision: int


def validate(spec: ConfigurationSpec, values: dict, *, required=True) -> dict:
    """Validate the deliberately small declarative scalar schema without coercion."""
    fields = {field.id: field for field in spec.fields}
    if set(values) - fields.keys():
        raise ProviderError("配置包含未声明字段")
    result = {}
    for name, field in fields.items():
        value = values.get(name)
        if value is None:
            value = field.default
        if value is None or (field.type == "string" and value == ""):
            if required and field.required:
                raise ProviderError(f"请先配置 {field.label}")
            continue
        expected = {"string": str, "integer": int, "boolean": bool}[field.type]
        if type(value) is not expected:
            raise ProviderError(f"{field.label} 的类型不正确")
        if isinstance(value, str):
            if len(value) > (field.max_length or 4096) or len(value) < (field.min_length or 0):
                raise ProviderError(f"{field.label} 的长度不符合要求")
            if field.secret and (value != value.strip() or any(c.isspace() for c in value)):
                raise ProviderError(f"{field.label} 格式不正确")
        if type(value) is int and (
            (field.minimum is not None and value < field.minimum)
            or (field.maximum is not None and value > field.maximum)
        ):
            raise ProviderError(f"{field.label} 超出允许范围")
        result[name] = value
    return result


class ProviderConfigurations:
    def __init__(self, engine, registry, credentials):
        self.engine, self.registry, self.credentials = engine, registry, credentials

    def head(self, provider):
        with self.engine.connect() as conn:
            return (
                conn.execute(select(configurations).where(configurations.c.provider == provider))
                .mappings()
                .first()
            )

    def current(self, provider):
        plugin = self.registry.get(provider)
        spec = plugin.manifest.configuration
        row = self.head(provider)
        if row:
            if row["schema_version"] != spec.schema_version:
                raise ProviderError("配置格式不受支持，请按当前规范重新配置")
            values = self.credentials.read_configuration(
                row["snapshot_ref"], provider, spec.schema_version, row["revision"]
            )
            return row["revision"], validate(spec, values, required=False)
        return 0, validate(spec, {}, required=False)

    def state(self, provider):
        spec = self.registry.get(provider).manifest.configuration
        error = None
        try:
            revision, values = self.current(provider)
        except ProviderError as exc:
            head = self.head(provider)
            revision = head["revision"] if head else 0
            values = validate(spec, {}, required=False)
            error = str(exc)
        secret_ids = {field.id for field in spec.fields if field.secret}
        try:
            validate(spec, values)
            configured = error is None
        except ProviderError:
            configured = False
        return ConfigurationState(
            provider=provider,
            revision=revision,
            schema_version=spec.schema_version,
            values={key: value for key, value in values.items() if key not in secret_ids},
            secret_fields=sorted(secret_ids & values.keys()),
            configured=configured,
            error=error,
        )

    def candidate(self, provider, body: ConfigurationUpdate):
        spec = self.registry.get(provider).manifest.configuration
        fields = {field.id: field for field in spec.fields}
        head = self.head(provider)
        revision = head["revision"] if head else 0
        if revision != body.expected_revision:
            raise Conflict("配置已在其他窗口更新，请刷新已保存配置后重试")
        try:
            current_revision, previous = self.current(provider)
            if current_revision != revision:
                raise Conflict("配置已更新，请刷新已保存配置后重试")
        except ProviderError:
            secrets = {key for key, field in fields.items() if field.secret}
            if (
                not secrets <= body.secrets.keys()
                or not (fields.keys() - secrets) <= body.values.keys()
            ):
                raise ProviderError(
                    "原配置无法读取，请重新填写全部配置和凭据；不能保留不可读的旧凭据"
                ) from None
            previous = {}
        if any(key not in fields or fields[key].secret for key in body.values):
            raise ProviderError("普通配置包含未声明字段或秘密字段")
        if any(key not in fields or not fields[key].secret for key in body.secrets):
            raise ProviderError("凭据配置包含未声明字段")
        values = {key: value for key, value in previous.items() if fields[key].secret}
        values.update(body.values)
        for key, secret in body.secrets.items():
            if secret is None:
                values.pop(key, None)
            else:
                values[key] = secret.get_secret_value()
        # Clearing credentials is a valid saved state, but is not runnable.
        return validate(spec, values, required=False)

    def apply(self, provider, body: ConfigurationUpdate):
        values = self.candidate(provider, body)
        spec = self.registry.get(provider).manifest.configuration
        ref = self.credentials.freeze_configuration(
            provider, spec.schema_version, values, body.expected_revision + 1
        )
        record = {
            "provider": provider,
            "revision": body.expected_revision + 1,
            "schema_version": spec.schema_version,
            "snapshot_ref": ref,
        }
        try:
            with self.engine.begin() as conn:
                if body.expected_revision == 0:
                    conn.execute(configurations.insert().values(**record))
                else:
                    changed = conn.execute(
                        configurations.update()
                        .where(
                            configurations.c.provider == provider,
                            configurations.c.revision == body.expected_revision,
                        )
                        .values(**record)
                    )
                    if changed.rowcount != 1:
                        raise Conflict("配置已更新，请刷新已保存配置后重试")
        except IntegrityError:
            raise Conflict("配置已更新，请刷新已保存配置后重试") from None
        return self.state(provider)

    def check(self, provider, body: ConfigurationUpdate):
        values = self.candidate(provider, body)
        plugin = self.registry.get(provider)
        validate(plugin.manifest.configuration, values)
        message = plugin.probe(values)
        head = self.head(provider)
        if (head["revision"] if head else 0) != body.expected_revision:
            raise Conflict("测试期间配置已更新，请刷新后重试")
        return ConfigurationCheck(message=message, revision=body.expected_revision)

    def freeze(self, provider):
        revision, values = self.current(provider)
        spec = self.registry.get(provider).manifest.configuration
        values = validate(spec, values)
        ref = self.credentials.freeze_configuration(provider, spec.schema_version, values, revision)
        return {"ref": ref, "revision": revision, "schema_version": spec.schema_version}

    def verification(self, provider):
        head = self.head(provider)
        revision = head["revision"] if head else 0
        with self.engine.connect() as conn:
            row = (
                conn.execute(
                    select(verification_records).where(verification_records.c.provider == provider)
                )
                .mappings()
                .first()
            )
        if not row:
            return VerificationState()
        status = row["status"] if row["revision"] == revision else "stale"
        messages = {
            "verified": "该配置通过连接测试；不代表所有数据接口均有权限",
            "failed": "连接验证失败，请检查配置、网络及接口权限",
            "stale": "配置已修改，之前的验证结果不再适用",
        }
        return VerificationState(
            status=status,
            checked_at=row["checked_at"],
            revision=row["revision"],
            message=messages[status],
        )

    def verify_saved(self, provider):
        revision, values = self.current(provider)
        plugin = self.registry.get(provider)
        validate(plugin.manifest.configuration, values)
        started = time.time()
        status = "verified"
        try:
            plugin.probe(values)
        except ProviderError:
            status = "failed"
        record = {
            "provider": provider,
            "revision": revision,
            "started_at": started,
            "checked_at": time.time(),
            "status": status,
        }
        with self.engine.begin() as conn:
            insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
            conn.execute(
                insert(verification_records)
                .values(**record)
                .on_conflict_do_update(
                    index_elements=["provider"],
                    set_=record,
                    where=verification_records.c.started_at <= started,
                )
            )
        return self.verification(provider)
