"""Offline physical snapshots; restoration is isolated and never overwrites a state directory."""

import contextlib
import json
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath
from typing import Literal

from asterion_bindings.file_archives import (
    ArchiveLimits,
    FileArchiveReader,
    FileArchiveWriter,
    StagedDirectory,
)
from asterion_bindings.files import atomic_write, file_lock, read_files
from pydantic import BaseModel, ConfigDict, Field

from asterion.platform.serialization import canonical
from asterion.runtime.desktop import (
    bootstrap,
    free_port,
    healthy,
    pg_command,
    pg_directory,
    pg_environment,
    runtime_settings,
    stop,
)

FORMAT = "asterion.desktop-backup"
MAX_BYTES = 500 * 1024**3
MAX_FILES = 1_000_000
CONFIG_BYTES = 64 * 1024
LIMITS = ArchiveLimits(
    bytes=MAX_BYTES, files=MAX_FILES, directories=MAX_FILES, metadata_bytes=128 * 1024**2
)


class FileEvidence(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    bytes: int = Field(ge=0, le=MAX_BYTES)
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")


class BackupManifest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    format: Literal["asterion.desktop-backup"]
    schema_version: int = Field(ge=1, le=1)
    created_at: float = Field(allow_inf_nan=False)
    platform: str = Field(min_length=1)
    machine: str = Field(min_length=1)
    postgres_major: str = Field(pattern=r"^[0-9]+$")
    files: dict[str, FileEvidence] = Field(max_length=MAX_FILES)
    directories: list[str] = Field(max_length=MAX_FILES)


def relative_file(name):
    path = PurePosixPath(name)
    if (
        not name
        or "\\" in name
        or path.is_absolute()
        or any(p in {"", ".", ".."} for p in name.split("/"))
    ):
        raise ValueError("备份包含不安全路径")
    if name != "desktop.json" and (not path.parts or path.parts[0] not in {"data", "postgres"}):
        raise ValueError("备份包含不支持的文件")
    return path


@contextlib.contextmanager
def offline(state):
    """Same locks as bootstrap/supervisor: no window may restart services during the copy."""
    with contextlib.ExitStack() as stack:
        for name in ("bootstrap.lock", "supervisor.lock"):
            try:
                stack.enter_context(file_lock(state / name, blocking=False))
            except BlockingIOError:
                raise ValueError("后台服务或其他维护操作仍在运行，请先停止服务") from None
        if (state / "postgres/postmaster.pid").exists():
            raise ValueError("数据库尚未停止，不能创建离线备份")
        yield


def create_backup(state: Path, destination: Path):
    state, destination = state.resolve(), destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("备份目标已存在，不覆盖已有文件")
    destination = destination.resolve()
    if destination.is_relative_to(state):
        raise ValueError("备份必须存放在本机状态目录之外")
    if not (state / "postgres/PG_VERSION").is_file() or not (state / "desktop.json").is_file():
        raise ValueError("本机数据库尚未初始化")
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (
        offline(state),
        FileArchiveWriter(
            state,
            destination,
            roots=["desktop.json", "postgres", "data"],
            excludes=["data/backups/"],
            limits=LIMITS,
            metadata_name="manifest.json",
        ) as archive,
    ):
        manifest = BackupManifest(
            format=FORMAT,
            schema_version=1,
            created_at=time.time(),
            platform=sys.platform,
            machine=platform.machine(),
            postgres_major=read_files(state, max_read_bytes=CONFIG_BYTES)
            .read("postgres/PG_VERSION")
            .decode()
            .strip(),
            **archive.catalog(),
        )
        return archive.commit(canonical(manifest.model_dump()))


def unpack(archive: FileArchiveReader, stage: StagedDirectory):
    """Validate desktop format and delegate byte checks to the fixed Rust mechanism."""
    manifest = BackupManifest.model_validate_json(archive.metadata())
    if manifest.platform != sys.platform or manifest.machine != platform.machine():
        raise ValueError("物理备份只支持相同操作系统和 CPU 架构")
    if not {"desktop.json", "postgres/PG_VERSION"} <= set(manifest.files):
        raise ValueError("备份缺少数据库或本机密钥配置")
    if "postgres/postmaster.pid" in manifest.files:
        raise ValueError("离线备份不能包含运行中的数据库进程标识")
    for name in manifest.files:
        relative_file(name)
    for name in manifest.directories:
        relative_file(name)
        if name == "desktop.json":
            raise ValueError("备份目录与文件冲突")
    archive.extract(stage, manifest.model_dump()["files"], manifest.directories)
    major = read_files(stage.path, max_read_bytes=CONFIG_BYTES).read("postgres/PG_VERSION")
    if major.decode().strip() != manifest.postgres_major:
        raise ValueError("数据库主版本与清单不一致")
    return manifest


class RestoreDatabaseUncertain(RuntimeError):
    """Cleanup cannot proceed until the isolated database is confirmed stopped."""


def stop_restore_database(state: Path, pg_root: Path):
    def stopped():
        status = subprocess.run(
            [
                str(pg_directory(pg_root, "bindir") / "pg_ctl"),
                "-D",
                str(state / "postgres"),
                "status",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            env=pg_environment(),
            timeout=10,
        )
        if status.returncode not in {0, 3}:
            raise RuntimeError("无法确认恢复数据库的运行状态")
        return status.returncode == 3

    try:
        if stopped():
            return
        for mode in ("fast", "immediate"):
            try:
                pg_command(
                    pg_root, "pg_ctl", "-D", str(state / "postgres"), "-m", mode, "-w", "stop"
                )
            except (OSError, subprocess.SubprocessError):
                # A command can fail after PostgreSQL has exited; inspect its
                # actual status before deciding whether another stop is needed.
                pass
            if stopped():
                return
        raise RuntimeError("恢复数据库仍在运行")
    except BaseException as error:
        raise RestoreDatabaseUncertain("未能确认恢复数据库已停止，不能清理恢复目录") from error


def validate_database(state, pg_root, *, quarantine=False):
    """Start only PostgreSQL, never API/workers. Verify catalog files and research outputs."""
    from asterion_bindings.database import create_engine
    from sqlalchemy import text

    inputs = read_files(state, max_read_bytes=CONFIG_BYTES)
    config = json.loads(inputs.read("desktop.json"))
    config["db_port"], config["api_port"] = free_port(), free_port()
    major = inputs.read("postgres/PG_VERSION").decode().strip()
    version_text = subprocess.run(
        [str(pg_directory(pg_root, "bindir") / "postgres"), "--version"],
        capture_output=True,
        text=True,
        check=True,
        env=pg_environment(),
        timeout=10,
    ).stdout
    found = re.search(r"PostgreSQL\) (\d+)\.", version_text)
    binary = found.group(1) if found else ""
    if binary != major:
        raise ValueError("恢复需要与备份相同的 PostgreSQL 主版本")
    options = shlex.join(
        [
            "-p",
            str(config["db_port"]),
            "-h",
            "127.0.0.1",
            "-c",
            f"data_directory={state / 'postgres'}",
            "-c",
            "unix_socket_directories=",
            "-c",
            "shared_preload_libraries=",
            "-c",
            "session_preload_libraries=",
            "-c",
            "local_preload_libraries=",
        ]
    )
    started = False
    try:
        started = True
        pg_command(
            pg_root,
            "pg_ctl",
            "-D",
            str(state / "postgres"),
            "-l",
            str(state / "restore-postgres.log"),
            "-o",
            options,
            "-w",
            "start",
        )
        engine = create_engine(runtime_settings(state, config).database_url)
        try:
            with engine.connect() as conn:
                conn.execute(text("SET TRANSACTION READ ONLY"))
                actual_directory = Path(conn.execute(text("SHOW data_directory")).scalar_one())
                if actual_directory.resolve() != (state / "postgres").resolve():
                    raise ValueError("恢复数据库未使用指定的隔离目录")
                from asterion_bindings.events import validate_journal
                from asterion_bindings.plugin_host import PluginHost
                from asterion_bindings.recovery import validate_checks

                from asterion.distribution import backup_inputs, builtin_plugins

                validate_journal(conn)
                plugins = PluginHost(builtin_plugins()).plugins
                with backup_inputs(conn, state, config["token"], plugins) as evidence:
                    counts = validate_checks(plugins, evidence)
            cancelled = 0
            if quarantine:
                from asterion.distribution import restore_inputs
                from asterion.runtime.restore import isolate_restore

                cancelled = isolate_restore(engine, plugins, restore_inputs)
        finally:
            engine.dispose()
    finally:
        if started:
            stop_restore_database(state, pg_root)
    # Preserve the master key, but assign isolated ports for a future explicit startup.
    with (state / "postgres/postgresql.conf").open("a") as config_file:
        config_file.write(
            f"\nport = {config['db_port']}\nlisten_addresses = '127.0.0.1'\nunix_socket_directories = ''\n"
        )
    atomic_write(state / "desktop.json", json.dumps(config).encode())
    return {
        **counts,
        "quarantined_tasks": cancelled,
    }


def restore_backup(archive: Path, target: Path, pg_root: Path):
    target = target.absolute()
    if target.exists() or target.is_symlink():
        raise ValueError("恢复目标必须是不存在的新目录，禁止覆盖当前数据")
    target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Resolve the host-selected parent once (for example macOS /var), then let
    # the native capability reject later link substitution or target creation.
    target = target.parent.resolve() / target.name
    with (
        FileArchiveReader(archive, limits=LIMITS, metadata_name="manifest.json") as source,
        StagedDirectory(target, max_entries=MAX_FILES * 2 + 1) as stage,
    ):
        unpack(source, stage)
        try:
            report = validate_database(stage.path, pg_root, quarantine=True)
        except RestoreDatabaseUncertain as error:
            retained = stage.preserve()
            raise RestoreDatabaseUncertain(
                f"未能确认恢复数据库已停止，已保留恢复现场：{retained}；目标目录尚未发布"
            ) from error
        atomic_write(
            stage.path / "restore-report.json",
            canonical(
                {
                    "status": "verified",
                    "verified_at": time.time(),
                    "archive_sha256": source.digest(),
                    "application_format": 1,
                    **report,
                }
            ),
            replace=False,
        )
        # Rust syncs and publishes the complete directory once without replacing a target.
        stage.commit()
    return {"status": "verified", "target": str(target), **report}


def managed_backup(state: Path, pg_root: Path, destination: Path | None = None):
    destination = (
        destination or state.parent / "Asterion Backups" / f"asterion-{time.time_ns()}.zip"
    )
    if not (state / "desktop.json").is_file() or not (state / "postgres/PG_VERSION").is_file():
        raise ValueError("本机数据库尚未初始化")
    config = json.loads(read_files(state, max_read_bytes=CONFIG_BYTES).read("desktop.json"))
    was_running = healthy(runtime_settings(state, config))
    try:
        if was_running:
            stop(state)
        result = create_backup(state, destination)
    finally:
        if was_running:
            bootstrap(state, pg_root)
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as temporary:
        report = restore_backup(destination, Path(temporary) / "restored", pg_root)
    return {
        "status": "verified",
        **result,
        "verification": {k: v for k, v in report.items() if k != "target"},
    }
