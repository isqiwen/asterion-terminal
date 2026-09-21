"""Host-owned environment selection; never replace a user's data directories."""

import fcntl
import json
import os
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

from asterion.runtime.backup import create_backup, offline, restore_backup, validate_database
from asterion.runtime.desktop import bootstrap, load_config, pg_command, stop


@contextmanager
def maintenance(host: Path, *, wait: bool = False):
    host.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (host / "maintenance.lock").open("a") as lock:
        deadline = time.monotonic() + (60 if wait else 0)
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise ValueError("本机维护正在进行，请稍后重试") from None
                time.sleep(0.1)
        yield


def read(host: Path) -> dict:
    path = host / "environment.json"
    if not path.exists():
        return {"active": str(host.resolve()), "previous": None, "pending": None}
    value = json.loads(path.read_text())
    if not Path(value["active"]).is_absolute():
        raise ValueError("环境记录无效")
    return value


def write(host: Path, value: dict):
    # Both file and directory are synced before proceeding to a destructive service transition.
    with tempfile.NamedTemporaryFile(dir=host, prefix=".environment-", delete=False) as file:
        temporary = Path(file.name)
        try:
            file.write(json.dumps(value).encode())
            file.flush()
            os.fsync(file.fileno())
            temporary.replace(host / "environment.json")
            fd = os.open(host, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            temporary.unlink(missing_ok=True)


def active(host: Path) -> Path:
    state = Path(read(host)["active"])
    selected = state != host.resolve() or (host / "environment.json").exists()
    if selected and not (
        (state / "desktop.json").is_file()
        and (state / "postgres/PG_VERSION").is_file()
        and (state / "data").is_dir()
    ):
        raise ValueError("当前环境目录不可用；请恢复目录后重试，禁止自动创建空环境")
    return state


def info(host: Path):
    state = active(host)
    if not (state / "desktop.json").exists():
        return None
    config = load_config(state)
    return {
        "api_url": f"http://127.0.0.1:{config['api_port']}",
        "token": config["token"],
        "data_directory": str(state / "data"),
    }


def status(host: Path):
    value = read(host)
    return {key: value.get(key) for key in ("active", "previous", "pending", "protection_backup")}


def stop_database(state: Path, pg_root: Path):
    # An interrupted validator may have left PostgreSQL running without a supervisor.
    if (state / "postgres/postmaster.pid").exists():
        pg_command(pg_root, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop")


def recover(host: Path, pg_root: Path):
    value = read(host)
    if not value.get("pending"):
        return
    target = Path(value["pending"])
    source = active(host)
    stop(target)
    stop_database(target, pg_root)
    stop(source)
    stop_database(source, pg_root)
    bootstrap(source, pg_root)
    write(host, {**value, "pending": None})


def switch(host: Path, pg_root: Path, target: Path | None = None):
    """Caller holds maintenance lock. A null target means rollback to previous."""
    recover(host, pg_root)
    value = read(host)
    source = active(host)
    rollback = target is None
    if rollback:
        if not value.get("previous"):
            raise ValueError("没有可回滚的环境")
        target = Path(value["previous"])
    assert target is not None
    target = target.resolve()
    if target.is_relative_to(source) or source.is_relative_to(target):
        raise ValueError("切换目标必须是独立目录")
    if (target / ".restore-incomplete").exists() or not (target / "desktop.json").is_file():
        raise ValueError("目标不是完整恢复目录")
    if not rollback:
        if target.is_relative_to(host.resolve()) or host.resolve().is_relative_to(target):
            raise ValueError("恢复目录必须位于宿主目录之外")
        report = json.loads((target / "restore-report.json").read_text())
        if report.get("status") != "verified" or report.get("application_format") != 1:
            raise ValueError("目标恢复验证未通过")
    # Refuse an already active target and unsafe filesystem entries before stopping the source.
    with offline(target):
        if any(path.is_symlink() for path in target.rglob("*")):
            raise ValueError("恢复目录不能包含符号链接")
    destination = host.parent / "Asterion Backups" / f"protection-{time.time_ns()}.zip"
    pending = {**value, "pending": str(target)}
    write(host, pending)
    try:
        stop(source)
        stop_database(source, pg_root)
        create_backup(source, destination)
        with tempfile.TemporaryDirectory(prefix="asterion-protection-") as temporary:
            restore_backup(destination, Path(temporary) / "verified", pg_root)
        pending = {**pending, "protection_backup": str(destination)}
        write(host, pending)
        with offline(target):
            verification = validate_database(target, pg_root, quarantine=True)
        bootstrap(target, pg_root)
        # Commit only after API and worker readiness. Startup will recover source until this write.
        write(
            host,
            {
                "active": str(target),
                "previous": str(source),
                "pending": None,
                "protection_backup": str(destination),
            },
        )
    except Exception:
        recover(host, pg_root)
        raise
    return {"status": "active", **status(host), "verification": verification}
