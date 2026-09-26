"""Terminal service and backup adapters for the fixed Rust environment coordinator."""

import json
import tempfile
from pathlib import Path

from asterion_bindings.environments import EnvironmentLease

from asterion.runtime.backup import create_backup, restore_backup, validate_database
from asterion.runtime.desktop import bootstrap, load_config, pg_command, stop


class EnvironmentHost:
    """Own one maintenance lease for a complete desktop lifecycle operation."""

    def __init__(self, host: Path, pg_root: Path | None = None, *, wait: bool = False):
        self.host = host.resolve()
        self.pg_root = pg_root
        self.wait = wait
        self._lease: EnvironmentLease | None = None

    def __enter__(self):
        self._lease = EnvironmentLease(
            self.host,
            self.host.parent / "Asterion Backups",
            {
                "required_files": ["desktop.json", "postgres/PG_VERSION"],
                "required_directories": ["data"],
                "offline_locks": ["bootstrap.lock", "supervisor.lock"],
            },
            wait=self.wait,
        )
        return self

    def __exit__(self, *_):
        self.lease.close()
        self._lease = None

    @property
    def lease(self) -> EnvironmentLease:
        if self._lease is None:
            raise ValueError("Environment host is not open")
        return self._lease

    def active(self) -> Path:
        return self.lease.active()

    def status(self) -> dict:
        return self.lease.status()

    def info(self):
        state = self.active()
        if not (state / "desktop.json").exists():
            return None
        config = load_config(state)
        return {
            "api_url": f"http://127.0.0.1:{config['api_port']}",
            "token": config["token"],
            "data_directory": str(state / "data"),
        }

    def recover(self) -> None:
        self.lease.recover(self._dispatch)

    def switch(self, target: Path | None = None) -> dict:
        return self.lease.switch(target, self._dispatch)

    def _dispatch(self, action: dict):
        if self.pg_root is None:
            raise ValueError("Database runtime is required for environment transitions")
        state = Path(action["state"])
        match action["operation"]:
            case "stop":
                stop(state)
                # A crashed validator can leave PostgreSQL without a supervisor.
                if (state / "postgres/postmaster.pid").exists():
                    pg_command(
                        self.pg_root,
                        "pg_ctl",
                        "-D",
                        str(state / "postgres"),
                        "-m",
                        "fast",
                        "-w",
                        "stop",
                    )
            case "start":
                bootstrap(state, self.pg_root)
            case "validate_target":
                if (state / "postgres/postmaster.pid").exists():
                    raise ValueError("数据库尚未停止，不能切换环境")
                if not action["rollback"]:
                    report = json.loads((state / "restore-report.json").read_text())
                    if report.get("status") != "verified" or report.get("application_format") != 1:
                        raise ValueError("目标恢复验证未通过")
            case "protect":
                destination = Path(action["destination"])
                create_backup(state, destination)
                with tempfile.TemporaryDirectory(prefix="asterion-protection-") as temporary:
                    restore_backup(destination, Path(temporary) / "verified", self.pg_root)
            case "verify":
                if (state / "postgres/postmaster.pid").exists():
                    raise ValueError("数据库尚未停止，不能验证环境")
                return validate_database(state, self.pg_root, quarantine=True)
            case _:
                raise ValueError("Unknown environment adapter operation")
        return None
