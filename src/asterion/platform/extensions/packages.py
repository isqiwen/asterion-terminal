"""Immutable local Python packages. Enabling is explicit trust in same-user code."""

import fcntl
import hashlib
import io
import json
import os
import re
import stat
import tempfile
import zipfile
from contextlib import contextmanager
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Manifest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    api_version: Literal[1]
    id: str = Field(pattern=r"^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$", max_length=100)
    version: str = Field(pattern=r"^[0-9]+\.[0-9]+\.[0-9]+$", max_length=40)
    title: str = Field(min_length=1, max_length=80)
    description: str = Field(max_length=1000)
    runtime: Literal["python"]
    entry: Literal["plugin.py"]
    trust: Literal["local-code"]
    requires: dict[str, str] = Field(max_length=16)
    contributions: dict[str, dict] = Field(min_length=1, max_length=8)

    @model_validator(mode="after")
    def dependencies(self):
        for identifier, version in self.requires.items():
            if (
                identifier == self.id
                or not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", identifier)
                or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)
            ):
                raise ValueError("Invalid plugin dependency")
        return self


class Receipt(BaseModel):
    model_config = ConfigDict(extra="forbid")
    manifest: Manifest
    digest: str = Field(pattern=r"^[0-9a-f]{64}$")
    enabled: bool = Field(strict=True)


def checked_archive(content):
    if len(content) > 16_000_000:
        raise ValueError("插件包超过 16 MB")
    try:
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            entries = archive.infolist()
            if len(entries) > 500 or sum(item.file_size for item in entries) > 32_000_000:
                raise ValueError("插件包展开后超过限制")
            files = {}
            for item in entries:
                name = item.filename
                if (
                    not name
                    or name.startswith("/")
                    or "\\" in name
                    or any(part in {"", ".", ".."} for part in name.split("/"))
                    or name in files
                    or item.is_dir()
                    or stat.S_ISLNK(item.external_attr >> 16)
                    or not re.fullmatch(r"[A-Za-z0-9_./-]+", name)
                ):
                    raise ValueError("插件包包含非法路径、链接或重复文件")
                if item.file_size > 8_000_000:
                    raise ValueError("插件文件超过限制")
                files[name] = archive.read(item)
            manifest = Manifest.model_validate_json(files["manifest.json"])
            if manifest.entry not in files:
                raise ValueError("插件入口不存在")
            if any(not name.endswith((".py", ".json", ".txt", ".md", ".csv")) for name in files):
                raise ValueError("本地 Python 插件只允许源码和文本资源")
            return manifest, files
    except (zipfile.BadZipFile, KeyError, UnicodeError):
        raise ValueError("插件包格式无效") from None


class Packages:
    def __init__(self, root: Path, validators):
        self.root = root
        self.validators = dict(validators)

    @contextmanager
    def locked(self):
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with (self.root / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
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
            receipt = Receipt.model_validate(record)
            if receipt.manifest.id != identifier:
                raise ValueError("插件安装记录身份不一致")
        return records

    def _save(self, records):
        fd, name = tempfile.mkstemp(dir=self.root, prefix=".state-")
        try:
            with os.fdopen(fd, "w") as target:
                json.dump({"api_version": 1, "packages": records}, target, ensure_ascii=False)
                target.flush()
                os.fsync(target.fileno())
            os.replace(name, self.root / "installed.json")
        finally:
            Path(name).unlink(missing_ok=True)

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
            known = Manifest.model_validate_json(receipt.read_bytes())
            if (
                known.id == manifest.id
                and known.version == manifest.version
                and receipt.parent.name != digest
            ):
                raise ValueError("相同插件版本已对应另一份内容，请使用新版本号")

    def _activation(self, records, manifest):
        for required, version in manifest.requires.items():
            dependency = records.get(required)
            if not dependency:
                raise ValueError(f"请先安装依赖插件 {required}，所需版本 {version}")
            if dependency["manifest"]["version"] != version:
                raise ValueError(f"依赖插件 {required} 版本不符，需要 {version}")
            if not dependency["enabled"]:
                raise ValueError(f"请先启用依赖插件 {required}（{version}）")
            self.resolve(dependency["digest"])

    def inspect(self, content):
        manifest, _ = checked_archive(content)
        self._validate(manifest)
        digest = hashlib.sha256(content).hexdigest()
        with self.locked():
            records = self._state()
            self._admit(records, manifest, digest)
            self._activation(records, manifest)
            candidate = records | {
                manifest.id: {"manifest": manifest.model_dump(), "digest": digest, "enabled": True}
            }
            self._dependencies(candidate)
        return {"manifest": manifest.model_dump(), "digest": digest}

    def install(self, content, *, enabled=False):
        manifest, files = checked_archive(content)
        self._validate(manifest)
        digest = hashlib.sha256(content).hexdigest()
        with self.locked():
            records = self._state()
            self._admit(records, manifest, digest)
            if enabled:
                self._activation(records, manifest)
            old = records.get(manifest.id)
            record = {
                "manifest": manifest.model_dump(),
                "digest": digest,
                "enabled": enabled or bool(old and old["enabled"]),
            }
            records[manifest.id] = record
            self._dependencies(records)
            objects = self.root / "objects"
            objects.mkdir(exist_ok=True, mode=0o700)
            target = objects / digest
            if not target.exists():
                with tempfile.TemporaryDirectory(dir=objects, prefix=".install-") as temporary:
                    staging = Path(temporary) / "package"
                    staging.mkdir()
                    for name, value in files.items():
                        file = staging / name
                        file.parent.mkdir(parents=True, exist_ok=True)
                        file.write_bytes(value)
                    (staging / ".archive").write_bytes(content)
                    os.replace(staging, target)
            self.resolve(digest)
            self._save(records)
            return record

    def list(self):
        with self.locked():
            return list(self._state().values())

    @staticmethod
    def _dependencies(records):
        visiting, visited = set(), set()

        def visit(identifier):
            if identifier in visiting:
                raise ValueError("插件依赖存在循环")
            if identifier in visited or identifier not in records:
                return
            visiting.add(identifier)
            for required in records[identifier]["manifest"]["requires"]:
                visit(required)
            visiting.remove(identifier)
            visited.add(identifier)

        for identifier in records:
            visit(identifier)

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
                self._activation(records, manifest)
            elif any(
                item["enabled"] and identifier in item["manifest"]["requires"]
                for item in records.values()
            ):
                raise ValueError("其他已启用插件依赖此插件，请先停用依赖者")
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
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("插件工件标识无效")
        target = self.root / "objects" / digest
        if target.is_symlink():
            raise ValueError("插件工件不能是链接")
        if any(path.is_symlink() for path in target.rglob("*")):
            raise ValueError("插件工件不能包含链接")
        content = (target / ".archive").read_bytes()
        if hashlib.sha256(content).hexdigest() != digest:
            raise ValueError("插件工件校验失败")
        manifest, files = checked_archive(content)
        actual = {str(p.relative_to(target)) for p in target.rglob("*") if p.is_file()}
        if actual != set(files) | {".archive"}:
            raise ValueError("插件工件文件清单已变化")
        for name, expected in files.items():
            path = target / name
            if path.read_bytes() != expected:
                raise ValueError("插件工件内容已变化")
        return manifest, target
