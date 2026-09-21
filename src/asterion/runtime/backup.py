"""Offline physical snapshots; restoration is isolated and never overwrites a state directory."""

import contextlib
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath

from asterion.platform.files import file_digest
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


def relative_file(name):
    path = PurePosixPath(name)
    if (
        not name
        or "\\" in name
        or path.is_absolute()
        or any(p in {".", ".."} for p in name.split("/"))
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
            handle = stack.enter_context((state / name).open("a"))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
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
        tempfile.TemporaryDirectory(
            prefix=".asterion-backup-", dir=destination.parent
        ) as temporary,
    ):
        output = Path(temporary) / "snapshot.zip"
        entries = {}
        directories = []
        total = 0
        with zipfile.ZipFile(
            output, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True
        ) as archive:
            roots = [state / "desktop.json", state / "postgres", state / "data"]
            for root in roots:
                if root.is_symlink():
                    raise ValueError("备份不支持符号链接")
                paths = [root] if root.is_file() else [root, *sorted(root.rglob("*"))]
                for path in paths:
                    if path.is_symlink():
                        raise ValueError("备份不支持符号链接或外部数据库表空间")
                    if path.is_dir():
                        directories.append(path.relative_to(state).as_posix())
                        continue
                    if not path.is_file():
                        raise ValueError("备份包含非普通文件")
                    name = path.relative_to(state).as_posix()
                    if name.startswith("data/backups/"):
                        continue
                    relative_file(name)
                    before = path.stat()
                    if total + before.st_size > MAX_BYTES or len(entries) >= MAX_FILES:
                        raise ValueError("备份超过当前支持的 500 GiB 或文件数量上限")
                    digest = hashlib.sha256()
                    with (
                        path.open("rb") as source,
                        archive.open(name, "w", force_zip64=True) as target,
                    ):
                        while chunk := source.read(1024 * 1024):
                            digest.update(chunk)
                            target.write(chunk)
                    after = path.stat()
                    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                        raise ValueError("备份期间文件发生变化，请关闭其他写入程序后重试")
                    total += before.st_size
                    entries[name] = {"bytes": before.st_size, "sha256": digest.hexdigest()}
            manifest = {
                "format": FORMAT,
                "schema_version": 1,
                "created_at": time.time(),
                "platform": sys.platform,
                "machine": platform.machine(),
                "postgres_major": (state / "postgres/PG_VERSION").read_text().strip(),
                "files": entries,
                "directories": directories,
            }
            archive.writestr("manifest.json", canonical(manifest))
        output.chmod(0o600)
        with output.open("rb") as completed:
            os.fsync(completed.fileno())
        # Atomic publication without overwriting a concurrently created backup.
        os.link(output, destination)
    return {
        "path": str(destination),
        "files": len(entries),
        "bytes": destination.stat().st_size,
        "sha256": file_digest(destination),
    }


def unpack(archive_path: Path, target: Path):
    """Extract only named regular files and check every byte before any database is started."""
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        if len(names) > MAX_FILES + 1 or len(names) != len(set(names)):
            raise ValueError("备份文件数量过大或包含重复路径")
        if (
            "manifest.json" not in names
            or archive.getinfo("manifest.json").file_size > 128 * 1024**2
        ):
            raise ValueError("备份清单缺失或过大")
        manifest = json.loads(archive.read("manifest.json"))
        if manifest.get("format") != FORMAT or manifest.get("schema_version") != 1:
            raise ValueError("不支持此备份格式")
        if (
            manifest.get("platform") != sys.platform
            or manifest.get("machine") != platform.machine()
        ):
            raise ValueError("物理备份只支持相同操作系统和 CPU 架构")
        entries = manifest["files"]
        if set(names) != set(entries) | {"manifest.json"}:
            raise ValueError("备份文件与清单不一致")
        if not {"desktop.json", "postgres/PG_VERSION"} <= set(entries):
            raise ValueError("备份缺少数据库或本机密钥配置")
        directories = manifest.get("directories", [])
        if len(directories) > MAX_FILES:
            raise ValueError("备份目录数量过大")
        for name in directories:
            relative_file(name)
            if name == "desktop.json" or name in entries:
                raise ValueError("备份目录与文件冲突")
            (target / name).mkdir(parents=True, exist_ok=True, mode=0o700)
        total = 0
        for name, expected in entries.items():
            relative_file(name)
            info = archive.getinfo(name)
            kind = stat.S_IFMT(info.external_attr >> 16)
            if (
                kind not in (0, stat.S_IFREG)
                or info.is_dir()
                or info.file_size != expected["bytes"]
            ):
                raise ValueError("备份包含链接、特殊文件或长度不一致")
            total += info.file_size
            if total > MAX_BYTES:
                raise ValueError("备份解压大小超过 500 GiB 上限")
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            digest = hashlib.sha256()
            with archive.open(name) as source, path.open("xb") as destination:
                path.chmod(0o600)
                while chunk := source.read(1024 * 1024):
                    digest.update(chunk)
                    destination.write(chunk)
            if digest.hexdigest() != expected["sha256"]:
                raise ValueError("备份文件校验和不一致")
        if (target / "postgres/PG_VERSION").read_text().strip() != manifest["postgres_major"]:
            raise ValueError("数据库主版本与清单不一致")
        return manifest


def validate_database(state, pg_root, *, quarantine=False):
    """Start only PostgreSQL, never API/workers. Verify catalog files and research outputs."""
    from sqlalchemy import create_engine, text

    config = json.loads((state / "desktop.json").read_text())
    config["db_port"], config["api_port"] = free_port(), free_port()
    major = (state / "postgres/PG_VERSION").read_text().strip()
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
    options = f"-p {config['db_port']} -h 127.0.0.1 -c unix_socket_directories='' -c shared_preload_libraries='' -c session_preload_libraries='' -c local_preload_libraries=''"
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
                from asterion.distribution import backup_inputs, builtin_plugins
                from asterion.platform.backup import validate_checks
                from asterion.platform.plugins import PluginHost

                plugins = PluginHost(builtin_plugins()).plugins
                counts = validate_checks(
                    plugins, backup_inputs(conn, state, config["token"], plugins)
                )
            cancelled = 0
            if quarantine:
                from asterion.distribution import restore_inputs
                from asterion.platform.backup import isolate_restore

                cancelled = isolate_restore(engine, plugins, restore_inputs)
        finally:
            engine.dispose()
    finally:
        if started:
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
            if status.returncode == 0:
                try:
                    pg_command(
                        pg_root, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop"
                    )
                except subprocess.CalledProcessError:
                    pg_command(
                        pg_root,
                        "pg_ctl",
                        "-D",
                        str(state / "postgres"),
                        "-m",
                        "immediate",
                        "-w",
                        "stop",
                    )
    # Preserve the master key, but assign isolated ports for a future explicit startup.
    with (state / "postgres/postgresql.conf").open("a") as config_file:
        config_file.write(
            f"\nport = {config['db_port']}\nlisten_addresses = '127.0.0.1'\nunix_socket_directories = ''\n"
        )
    (state / "desktop.json").write_text(json.dumps(config))
    (state / "desktop.json").chmod(0o600)
    return {
        **counts,
        "quarantined_tasks": cancelled,
    }


def restore_backup(archive: Path, target: Path, pg_root: Path):
    target = target.absolute()
    if target.exists() or target.is_symlink():
        raise ValueError("恢复目标必须是不存在的新目录，禁止覆盖当前数据")
    target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix=".asterion-restore-", dir=target.parent) as temporary:
        staged = Path(temporary) / "state"
        staged.mkdir(mode=0o700)
        unpack(archive, staged)
        report = validate_database(staged, pg_root, quarantine=True)
        # mkdir reserves the destination; do not replace an existing directory even if empty.
        (staged / "restore-report.json").write_bytes(
            canonical(
                {
                    "status": "verified",
                    "verified_at": time.time(),
                    "archive_sha256": file_digest(archive),
                    "application_format": 1,
                    **report,
                }
            )
        )
        (staged / "restore-report.json").chmod(0o600)
        target.mkdir(mode=0o700)
        marker = target / ".restore-incomplete"
        marker.touch(mode=0o600)
        try:
            for child in staged.iterdir():
                child.rename(target / child.name)
            marker.unlink()
        except BaseException:
            shutil.rmtree(target)
            raise
    return {"status": "verified", "target": str(target), **report}


def managed_backup(state: Path, pg_root: Path, destination: Path | None = None):
    destination = (
        destination or state.parent / "Asterion Backups" / f"asterion-{time.time_ns()}.zip"
    )
    if not (state / "desktop.json").is_file() or not (state / "postgres/PG_VERSION").is_file():
        raise ValueError("本机数据库尚未初始化")
    config = json.loads((state / "desktop.json").read_text())
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
