"""Immutable local Python packages. Enabling is explicit trust in same-user code."""

import json
import re
from contextlib import contextmanager
from pathlib import Path

from asterion_bindings.files import atomic_write, file_lock
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from asterion_plugin_sdk import packages as package_contract


class Receipt(BaseModel):
    model_config = ConfigDict(extra="forbid")
    manifest: package_contract.PackageManifest
    digest: str = Field(pattern=r"^[0-9a-f]{64}$")
    enabled: bool = Field(strict=True)


class Packages:
    def __init__(self, root: Path, validators):
        self.root = root
        self.validators = dict(validators)

    @contextmanager
    def locked(self):
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with file_lock(self.root / ".lock"):
            yield

    def _state(self):
        path = self.root / "installed.json"
        if not path.exists():
            return {}
        value = json.loads(path.read_text())
        if (
            not isinstance(value, dict)
            or set(value) != {"api_version", "packages"}
            or value["api_version"] != 1
        ):
            raise ValueError("插件安装记录不符合当前契约")
        records = value["packages"]
        if not isinstance(records, dict):
            raise ValueError("插件安装记录不符合当前契约")  # noqa: TRY004 - serialized input error
        for identifier, record in records.items():
            try:
                receipt = Receipt.model_validate(record)
            except ValidationError:
                raise ValueError(
                    "插件安装记录不符合当前契约：只支持策略插件，外部数据源插件已不再支持"
                ) from None
            if receipt.manifest.id != identifier:
                raise ValueError("插件安装记录身份不一致")
        return records

    def _save(self, records):
        atomic_write(
            self.root / "installed.json",
            json.dumps({"api_version": 1, "packages": records}, ensure_ascii=False).encode(),
        )

    def _validate(self, manifest):
        if manifest.id.startswith("asterion."):
            raise ValueError("该插件命名空间由内置发行版保留")
        for name, contribution in manifest.contributions.items():
            if name not in self.validators:
                raise ValueError("未支持的插件扩展点")
            self.validators[name](manifest, contribution)

    def _admit(self, records, manifest, digest):
        old = records.get(manifest.id)
        if old and old["digest"] == digest:
            self.resolve(digest)
            return
        if old:
            raise ValueError("请先停用并移除当前插件，再安装新工件；已引用工件会保留")
        for existing in records.values():
            for kind, contribution in manifest.contributions.items():
                other = existing["manifest"]["contributions"].get(kind)
                if other and other.get("id") == contribution.get("id"):
                    raise ValueError("插件贡献标识已被另一个安装占用")
        # Same release identity cannot silently acquire different content, even after removal.
        for receipt in self.root.glob("objects/*/manifest.json"):
            if not re.fullmatch(r"[0-9a-f]{64}", receipt.parent.name):
                continue
            known, _ = self.resolve(receipt.parent.name)
            if (
                known.id == manifest.id
                and known.version == manifest.version
                and receipt.parent.name != digest
            ):
                raise ValueError("相同插件版本已对应另一份内容，请使用新版本号")

    def inspect(self, content):
        manifest, archive = package_contract.checked_package(content)
        self._validate(manifest)
        digest = archive.digest
        with self.locked():
            self._admit(self._state(), manifest, digest)
        return {"manifest": manifest.model_dump(), "digest": digest}

    def install(self, content, *, enabled=False):
        manifest, archive = package_contract.checked_package(content)
        self._validate(manifest)
        digest = archive.digest
        with self.locked():
            records = self._state()
            self._admit(records, manifest, digest)
            old = records.get(manifest.id)
            record = {
                "manifest": manifest.model_dump(),
                "digest": digest,
                "enabled": enabled or bool(old and old["enabled"]),
            }
            records[manifest.id] = record
            objects = self.root / "objects"
            objects.mkdir(exist_ok=True, mode=0o700)
            archive.publish(objects)
            self._save(records)
            return record

    def list(self):
        with self.locked():
            return list(self._state().values())

    def enabled(self, identifier, digest):
        return any(
            record["manifest"]["id"] == identifier
            and record["digest"] == digest
            and record["enabled"]
            for record in self.list()
        )

    def select(self, identifier, digest, enabled):
        with self.locked():
            records = self._state()
            old = records.get(identifier)
            if old is None or old["digest"] != digest:
                raise ValueError("插件状态已变化，请刷新后重试")
            if enabled:
                manifest, _ = self.resolve(digest)
                self._validate(manifest)
            old["enabled"] = enabled
            self._save(records)
            return old

    def remove(self, identifier, digest):
        with self.locked():
            records = self._state()
            old = records.get(identifier)
            if old is None or old["digest"] != digest:
                raise ValueError("插件状态已变化，请刷新后重试")
            if old["enabled"]:
                raise ValueError("请先停用插件")
            del records[identifier]
            self._save(records)
        return {"status": "removed", "retained": digest}

    def resolve(self, digest):
        try:
            manifest, _ = package_contract.load_package(self.root / "objects", digest)
        except ValueError as error:
            raise ValueError("插件工件校验失败，格式或内容已变化") from error
        return manifest, self.root / "objects" / digest
