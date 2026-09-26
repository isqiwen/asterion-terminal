"""Desktop bootstrap. launchd/systemd own services independently of windows."""

import contextlib
import json
import os
import plistlib
import secrets
import shlex
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import quote

import httpx
from asterion_bindings.file_archives import StagedDirectory
from asterion_bindings.files import atomic_write, file_lock
from pydantic_settings import SettingsConfigDict

from asterion.platform.config import Settings
from asterion.runtime.build_identity import runtime_identity


class DesktopSettings(Settings):
    model_config = SettingsConfigDict(env_prefix="ASTERION_", env_file=None)


LABEL = "me.asterion.terminal.backend"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def load_config(state: Path) -> dict:
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    state.chmod(0o700)
    path = state / "desktop.json"
    if path.exists():
        return json.loads(path.read_text())
    config = {
        "version": 1,
        "api_port": free_port(),
        "db_port": free_port(),
        "token": secrets.token_urlsafe(32),
        "db_password": secrets.token_urlsafe(32),
    }
    atomic_write(path, json.dumps(config).encode(), replace=False)
    return config


def runtime_settings(state: Path, config: dict) -> Settings:
    return DesktopSettings(
        database_url=(
            f"postgresql://asterion:{quote(config['db_password'])}"
            f"@127.0.0.1:{config['db_port']}/asterion"
        ),
        data_root=state / "data",
        api_url=f"http://127.0.0.1:{config['api_port']}",
        token=config["token"],
        require_account=True,
    )


def executable() -> list[str]:
    return [sys.executable, "-m", "asterion.runtime.cli"]


def server_executable() -> Path:
    """The Rust HTTP entry is installed beside the runtime interpreter."""
    path = Path(sys.executable).parent / "asterion-server"
    if not path.is_file():
        raise RuntimeError("运行环境缺少 asterion-server，请重新完成首次设置")
    return path


def child_environment(settings: Settings) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("ASTERION_")}
    env.update(
        ASTERION_DATABASE_URL=settings.database_url,
        ASTERION_DATA_ROOT=str(settings.data_root),
        ASTERION_API_URL=settings.api_url,
        ASTERION_TOKEN=settings.token,
        ASTERION_REQUIRE_ACCOUNT=str(settings.require_account).lower(),
        PYTHONDONTWRITEBYTECODE="1",
    )
    return env


def entry_environment(settings: Settings) -> dict:
    """The Rust entry authorizes requests and owns accounts, the task queue
    operations, event replay and scope credentials."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("ASTERION_")}
    env.update(
        ASTERION_TOKEN=settings.token,
        ASTERION_DATABASE_URL=settings.database_url,
        ASTERION_DATA_ROOT=str(settings.data_root),
        ASTERION_ACCOUNT_VERIFICATION=settings.account_verification,
        ASTERION_REQUIRE_ACCOUNT=str(settings.require_account).lower(),
        ASTERION_LEASE_SECONDS=str(settings.lease_seconds),
    )
    return env


def healthy(settings: Settings) -> bool:
    try:
        with httpx.Client(trust_env=False, timeout=1) as client:
            response = client.get(
                settings.api_url + "/api/v1/health",
                headers={"Authorization": f"Bearer {settings.token}"},
            )
            return response.status_code == 200 and response.json().get("status") == "ready"
    except (httpx.HTTPError, ValueError):
        return False


def service_plist(state: Path, pg_root: Path, build_id: str | None = None) -> dict:
    return {
        "Label": LABEL,
        "ProgramArguments": executable()
        + ["desktop-supervise", "--state", str(state), "--pg-root", str(pg_root)],
        "WorkingDirectory": str(state),
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 5,
        "ProcessType": "Background",
        "EnvironmentVariables": {
            "PYTHONDONTWRITEBYTECODE": "1",
            "LC_ALL": "C",
            "LANG": "C",
            "ASTERION_RUNTIME_BUILD": build_id or runtime_identity(pg_root),
        },
        "StandardOutPath": str(state / "service.log"),
        "StandardErrorPath": str(state / "service.log"),
    }


def launchctl(*arguments: str, check=True):
    result = subprocess.run(
        ["/bin/launchctl", *arguments], capture_output=True, text=True, check=False, timeout=30
    )
    if check and result.returncode:
        raise RuntimeError(
            f"无法管理本机 launchd 服务（{arguments[0]}，退出码 {result.returncode}）："
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result


def systemctl(*arguments: str, check=True):
    result = subprocess.run(
        ["/usr/bin/systemctl", "--user", *arguments],
        capture_output=True,
        text=True,
        check=False,
        timeout=150,
    )
    if check and result.returncode:
        raise RuntimeError(f"无法管理用户级 systemd 服务：{result.stderr.strip()}")
    return result


def systemd_quote(value: str, *, command=False) -> str:
    # Unit specifiers and ExecStart environment substitution are not shell quoting.
    value = value.replace("%", "%%")
    if command:
        value = value.replace("$", "$$")
    return json.dumps(value, ensure_ascii=False)


def service_unit(state: Path, pg_root: Path, build_id: str | None = None) -> str:
    if any(c in str(state) + str(pg_root) for c in "\n\r"):
        raise ValueError("服务路径不能包含换行符")
    arguments = executable() + [
        "desktop-supervise",
        "--state",
        str(state),
        "--pg-root",
        str(pg_root),
    ]
    return (
        "[Unit]\nDescription=Asterion local backend\n\n[Service]\nType=simple\n"
        f"ExecStart={' '.join(systemd_quote(a, command=True) for a in arguments)}\n"
        f"WorkingDirectory={str(state).replace('%', '%%')}\n"
        "Environment=PYTHONDONTWRITEBYTECODE=1 LC_ALL=C LANG=C\n"
        f"Environment=ASTERION_RUNTIME_BUILD={build_id or runtime_identity(pg_root)}\n"
        "Restart=always\nRestartSec=5\nKillMode=mixed\nTimeoutStopSec=130\n"
        "UMask=0077\n"
        f"StandardOutput=append:{str(state / 'service.log').replace('%', '%%')}\n"
        f"StandardError=append:{str(state / 'service.log').replace('%', '%%')}\n"
    )


def start_service(state: Path, pg_root: Path, build_id: str | None = None) -> None:
    if sys.platform == "linux":
        config_home = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
        path = config_home / "systemd/user" / f"{LABEL}.service"
        path.parent.mkdir(parents=True, exist_ok=True)
        desired = service_unit(state, pg_root, build_id)
        changed = not path.exists() or path.read_text() != desired
        if changed:
            if path.exists():
                systemctl("stop", f"{LABEL}.service")
                wait_stopped(state)
            atomic_write(path, desired.encode())
        # Reload even when unchanged: a previous attempt may have failed after writing.
        systemctl("daemon-reload")
        systemctl("start", f"{LABEL}.service")
        return
    domain = f"gui/{os.getuid()}"
    path = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
    path.parent.mkdir(parents=True, exist_ok=True)
    desired = service_plist(state, pg_root, build_id)
    current = plistlib.loads(path.read_bytes()) if path.exists() else None
    installed = launchctl("print", f"{domain}/{LABEL}", check=False).returncode == 0
    if current != desired:
        if installed:
            launchctl("bootout", f"{domain}/{LABEL}")
            # bootout can return before launchd has removed the old registration.
            deadline = time.monotonic() + 40
            while launchctl("print", f"{domain}/{LABEL}", check=False).returncode == 0:
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        f"旧版后台服务仍在停止，请稍后重试。诊断日志：{state / 'service.log'}"
                    )
                time.sleep(0.2)
            wait_stopped(state)
            installed = False
        atomic_write(path, plistlib.dumps(desired))
    if not installed:
        # A removed registration may briefly remain unavailable for reuse.
        for attempt in range(5):
            result = launchctl("bootstrap", domain, str(path), check=False)
            if result.returncode == 0:
                return
            if result.returncode != 5 or attempt == 4:
                raise RuntimeError(
                    f"无法注册本机后台服务（退出码 {result.returncode}）："
                    f"{result.stderr.strip() or result.stdout.strip()}。"
                    f"服务配置：{path}；诊断日志：{state / 'service.log'}"
                )
            time.sleep(0.5)


def bootstrap(state: Path, pg_root: Path) -> dict:
    if sys.platform not in {"darwin", "linux"}:
        raise RuntimeError("当前桌面运行包仅支持 macOS 和 Linux")
    state, pg_root = state.resolve(), pg_root.resolve()
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    with file_lock(state / "bootstrap.lock"):
        config = load_config(state)
        settings = runtime_settings(state, config)
        build_id = runtime_identity(pg_root)
        start_service(state, pg_root, build_id)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if healthy(settings) and worker_ready(state) and running_build(state) == build_id:
                return {
                    "api_url": settings.api_url,
                    "token": settings.token,
                    "data_directory": str(settings.data_root),
                }
            time.sleep(0.4)
        raise RuntimeError(f"本机服务尚未就绪，请重试。诊断日志：{state / 'service.log'}")


def running_build(state: Path) -> str | None:
    try:
        status = json.loads((state / "runtime-status.json").read_text())
        return status["build_id"] if 0 <= time.time() - status["observed_at"] < 5 else None
    except (OSError, ValueError, TypeError, KeyError):
        return None


def worker_ready(state: Path) -> bool:
    try:
        status = json.loads((state / "runtime-status.json").read_text())
        return status["worker"] == "running" and 0 <= time.time() - status["observed_at"] < 5
    except (OSError, ValueError, TypeError, KeyError):
        return False


def stop(state: Path) -> None:
    if sys.platform == "linux":
        systemctl("stop", f"{LABEL}.service")
    else:
        launchctl("bootout", f"gui/{os.getuid()}/{LABEL}", check=False)
    wait_stopped(state)


def wait_stopped(state: Path) -> None:
    # Never install a replacement while the previous supervisor still owns the database.
    try:
        with file_lock(state / "supervisor.lock", timeout=40):
            pass
    except TimeoutError:
        raise RuntimeError("后台仍在停止，请稍后重试") from None


def pg_directory(pg_root: Path, key: str) -> Path:
    layout = (
        {"bindir": "lib/postgresql/17/bin", "sharedir": "share/postgresql/17"}
        if sys.platform == "linux"
        else {"bindir": "bin", "sharedir": "share/postgresql"}
    )
    return pg_root / layout[key]


def pg_environment(env=None) -> dict:
    result = os.environ.copy() if env is None else env.copy()
    return result | {"LC_ALL": "C", "LANG": "C"}


def pg_command(pg_root: Path, name: str, *arguments: str, env=None):
    return subprocess.run(
        [str(pg_directory(pg_root, "bindir") / name), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=pg_environment(env),
        timeout=90,
    )


def initialize_postgres(state: Path, pg_root: Path, config: dict):
    pgdata = state / "postgres"
    if not (pgdata / "PG_VERSION").exists():
        if pgdata.exists() and any(pgdata.iterdir()):
            raise RuntimeError("数据库初始化曾中断；保留原目录，请检查本机诊断日志")
        password_file = state / "init-password"
        atomic_write(password_file, config["db_password"].encode())
        try:
            with StagedDirectory(pgdata, max_entries=1_000_000) as staging:
                pg_command(
                    pg_root,
                    "initdb",
                    "-D",
                    str(staging.path),
                    "-U",
                    "asterion",
                    "-E",
                    "UTF8",
                    "--locale=C",
                    "-L",
                    str(pg_directory(pg_root, "sharedir")),
                    "--auth=scram-sha-256",
                    f"--pwfile={password_file}",
                )
                with (staging.path / "postgresql.conf").open("a") as config_file:
                    config_file.write(
                        f"\nlisten_addresses = '127.0.0.1'\nport = {config['db_port']}\n"
                        "unix_socket_directories = ''\nshared_buffers = '64MB'\n"
                        "max_connections = 30\n"
                    )
                staging.commit()
        finally:
            password_file.unlink(missing_ok=True)
    # Status tests the PID and database identity in this exact cluster directory.
    status = subprocess.run(
        [str(pg_directory(pg_root, "bindir") / "pg_ctl"), "-D", str(pgdata), "status"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        env=pg_environment(),
    )
    if status.returncode != 0:
        pg_command(
            pg_root,
            "pg_ctl",
            "-D",
            str(pgdata),
            "-l",
            str(state / "postgres.log"),
            "-o",
            shlex.join(["-c", f"data_directory={pgdata.resolve()}"]),
            "-w",
            "start",
        )
    from asterion_bindings.database import create_engine
    from sqlalchemy import text

    admin = create_engine(
        f"postgresql://asterion:{quote(config['db_password'])}"
        f"@127.0.0.1:{config['db_port']}/postgres"
    )
    try:
        with admin.connect().execution_options(isolation_level="AUTOCOMMIT") as conn:
            if not conn.execute(
                text("SELECT 1 FROM pg_database WHERE datname = 'asterion'")
            ).first():
                conn.execute(text("CREATE DATABASE asterion"))
    finally:
        admin.dispose()


def supervise(state: Path, pg_root: Path):
    from asterion_bindings.supervisor import Supervisor

    state, pg_root = state.resolve(), pg_root.resolve()
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    with contextlib.ExitStack() as lifetime:
        try:
            lifetime.enter_context(file_lock(state / "supervisor.lock", blocking=False))
        except BlockingIOError:
            return
        build_id = runtime_identity(pg_root)
        expected = os.environ.get("ASTERION_RUNTIME_BUILD")
        if expected is not None and expected != build_id:
            raise RuntimeError("应用运行文件已变化，请重新打开应用完成后台更新")
        config = load_config(state)
        settings = runtime_settings(state, config)
        environment = child_environment(settings)
        command = executable()
        # Clients reach only the Rust entry on api_port. The Python process that
        # still owns unmigrated operations listens on a per-start loopback port.
        internal_port = free_port()
        supervisor = Supervisor(
            [
                {
                    "name": "server",
                    "program": str(server_executable()),
                    "arguments": [
                        "--listen",
                        f"127.0.0.1:{config['api_port']}",
                        "--upstream",
                        f"http://127.0.0.1:{internal_port}",
                    ],
                    "environment": entry_environment(settings),
                    "cwd": str(state),
                },
                *(
                    {
                        "name": role,
                        "program": command[0],
                        "arguments": command[1:]
                        + [role]
                        + (["--port", str(internal_port)] if role == "serve" else []),
                        "environment": environment,
                        "cwd": str(state),
                    }
                    for role in ("serve", "worker")
                ),
            ],
            state / "runtime-status.json",
            build_id,
        )

        def shutdown(*_):
            supervisor.request_stop()

        previous_signals = {
            number: signal.signal(number, shutdown) for number in (signal.SIGTERM, signal.SIGINT)
        }
        try:
            initialize_postgres(state, pg_root, config)
            from asterion.platform.store import database
            from asterion.runtime.initialize import initialize

            engine = database(settings.database_url)
            try:
                initialize(settings, engine)
            finally:
                engine.dispose()
            supervisor.run()
        except subprocess.CalledProcessError:
            from asterion_bindings.diagnostics import record_event

            record_event(settings.data_root / ".diagnostics", "supervisor", "subprocess_failed")
            raise
        finally:
            # The Rust supervisor reclaims all owned children before this returns.
            try:
                with contextlib.suppress(subprocess.CalledProcessError):
                    pg_command(
                        pg_root, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop"
                    )
            finally:
                for number, handler in previous_signals.items():
                    signal.signal(number, handler)
