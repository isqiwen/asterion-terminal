"""Check native build dependencies and offer installation in interactive terminals."""

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

LINUX_PACKAGES = (
    "build-essential",
    "pkg-config",
    "libwebkit2gtk-4.1-dev",
    "libayatana-appindicator3-dev",
    "librsvg2-dev",
    "libssl-dev",
    "patchelf",
    "postgresql-17",
    "libpq-dev",
    "python3-dev",
)


def execute(*args: str, capture=False):
    return subprocess.run(
        list(args),
        check=False,
        text=True,
        capture_output=capture,
        env=os.environ | {"LC_ALL": "C", "LANG": "C"},
    )


def confirm_install(packages: list[str], manager: str, interactive: bool) -> None:
    print(f"Missing {manager} dependencies: {', '.join(packages)}", flush=True)
    ci = os.environ.get("CI", "").lower() not in {"", "0", "false", "no"}
    if not interactive or ci or not sys.stdin.isatty():
        raise SystemExit(
            "Dependency installation requires an interactive terminal outside CI. "
            "Install the prerequisites in README.md and retry."
        )
    try:
        answer = input(f"Install these packages using {manager}? [y/N] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        answer = ""
    if answer not in {"y", "yes"}:
        raise SystemExit("Installation cancelled. No packages were installed.")


def apt_missing(packages: list[str]) -> list[str]:
    missing = []
    for package in packages:
        result = execute("dpkg-query", "-W", "-f=${Status}", package, capture=True)
        if result.returncode or result.stdout.strip() != "install ok installed":
            missing.append(package)
    return missing


def ensure_linux(*, runtime: bool, desktop: bool, interactive: bool) -> None:
    distribution = platform.freedesktop_os_release()
    family = {distribution.get("ID", ""), *distribution.get("ID_LIKE", "").split()}
    if not family.intersection({"debian", "ubuntu"}):
        raise SystemExit(
            "Automatic dependency setup supports Debian/Ubuntu only. "
            "Install the Linux prerequisites manually and use --no-install."
        )
    packages = list(LINUX_PACKAGES) if desktop else []
    if not runtime:
        packages = [
            p
            for p in packages
            if p
            not in {
                "patchelf",
                "postgresql-17",
                "libpq-dev",
                "python3-dev",
            }
        ]
    elif not desktop:
        packages = ["patchelf", "postgresql-17", "libpq-dev", "python3-dev"]
    missing = apt_missing(packages)
    if not missing:
        return
    confirm_install(missing, "sudo apt-get", interactive)
    if not shutil.which("sudo"):
        raise SystemExit(
            "sudo is unavailable. Ask your administrator to install the listed packages."
        )
    if execute("sudo", "apt-get", "update").returncode:
        raise SystemExit("Could not refresh package metadata. Fix the apt error above and retry.")
    # Only use configured repositories; never add sources or signing keys automatically.
    unavailable = []
    for package in missing:
        result = execute("apt-cache", "policy", package, capture=True)
        candidates = [
            line.split(":", 1)[1].strip()
            for line in result.stdout.splitlines()
            if line.strip().startswith("Candidate:")
        ]
        if result.returncode or not candidates or candidates[0] == "(none)":
            unavailable.append(package)
    if unavailable:
        raise SystemExit(
            "Packages unavailable in configured repositories: "
            + ", ".join(unavailable)
            + ". No packages were installed or repositories added. "
            "For PostgreSQL 17, configure the official PGDG repository (README.md)."
        )
    if execute("sudo", "apt-get", "install", "-y", "--no-remove", *missing).returncode:
        raise SystemExit(
            "System dependency installation failed. Fix the apt error above and retry."
        )
    remaining = apt_missing(packages)
    if remaining:
        raise SystemExit("Dependencies still missing after installation: " + ", ".join(remaining))


def mac_postgres_source() -> Path:
    override = os.environ.get("ASTERION_PG_SOURCE")
    if override:
        return Path(override).expanduser().resolve()
    if shutil.which("brew"):
        result = execute("brew", "--prefix", capture=True)
        if not result.returncode:
            return Path(result.stdout.strip()) / "opt/postgresql@17"
    return Path("/opt/homebrew/opt/postgresql@17")


def ensure_macos(*, runtime: bool, desktop: bool, interactive: bool) -> None:
    if desktop and execute("/usr/bin/xcode-select", "-p", capture=True).returncode:
        raise SystemExit(
            "Install Apple's Command Line Tools using xcode-select --install, "
            "complete the installer, and retry."
        )
    if not runtime:
        return
    source = mac_postgres_source()
    if all((source / "bin" / tool).is_file() for tool in ("postgres", "initdb", "pg_ctl")):
        return
    if os.environ.get("ASTERION_PG_SOURCE"):
        raise SystemExit(f"ASTERION_PG_SOURCE does not contain a PostgreSQL runtime: {source}")
    if not shutil.which("brew"):
        raise SystemExit(
            "Homebrew is required to install PostgreSQL 17. Install Homebrew and retry."
        )
    confirm_install(["postgresql@17"], "Homebrew", interactive)
    if execute("brew", "install", "postgresql@17").returncode:
        raise SystemExit("Homebrew installation failed. Fix the error above and retry.")
    if not all((source / "bin" / tool).is_file() for tool in ("postgres", "initdb", "pg_ctl")):
        raise SystemExit(f"PostgreSQL runtime is still missing after installation: {source}")
