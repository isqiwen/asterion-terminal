#!/usr/bin/env python3
"""Unified offline cold-backup, verification, and restore tool for Asterion Linux services.

Supports Market, Warehouse, and Trading installations:
  - Enforces cold backup (refuses while systemd units are active or service locks are held).
  - Never copies host-bound systemd encrypted credentials (/etc/credstore.encrypted).
  - Preserves hard links (Warehouse research/source Parquet files) and strict file modes (TLS keys).
  - Embeds a per-file SHA-256 manifest (`backup-manifest.json`) and verifies SQLite / Warehouse
    integrity before restoring.
"""
import argparse
import datetime
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import pwd
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile

SERVICES = ("market", "warehouse", "trading")
DEFAULT_INSTALL_DIRS = {
    "market": Path("/opt/asterion/market"),
    "warehouse": Path("/opt/asterion/warehouse"),
    "trading": Path("/opt/asterion/trading"),
}
MANIFEST_NAME = "backup-manifest.json"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def _check_systemd_stopped(service: str, skip: bool):
    if skip or not shutil.which("systemctl"):
        return
    if service == "trading":
        proc = subprocess.run(
            ["systemctl", "list-units", "--type=service", "--state=running,activating,reloading",
             "--no-legend", "asterion-trading@*.service"],
            capture_output=True, text=True,
        )
        running = [line.split()[0] for line in proc.stdout.splitlines() if line.strip()]
        if running:
            raise RuntimeError(
                f"refusing cold operation while Trading services are active: {', '.join(running)}"
            )
        return
    unit = f"asterion-{service}.service"
    proc = subprocess.run(["systemctl", "is-active", "--quiet", unit])
    if proc.returncode == 0:
        raise RuntimeError(f"refusing cold operation while {unit} is active; stop it first")


def _try_locks(lock_paths: list[Path]) -> list:
    held = []
    try:
        for path in lock_paths:
            if not path.is_file():
                continue
            fd = os.open(path, os.O_RDONLY)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                os.close(fd)
                raise RuntimeError(
                    f"lock is held by a running process: {path}; stop the service before cold backup/restore"
                ) from None
            held.append(fd)
        return held
    except BaseException:
        for fd in held:
            os.close(fd)
        raise


def _release_locks(fds: list):
    for fd in fds:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def _service_data_dir(service: str, install_dir: Path) -> Path:
    if service == "warehouse":
        cfg = install_dir / "config" / "warehouse.json"
        if cfg.is_file():
            data = json.loads(cfg.read_text()).get("data")
            if data:
                return Path(data)
    elif service == "market":
        cfg = install_dir / "config" / "market.json"
        if cfg.is_file():
            data = json.loads(cfg.read_text()).get("data")
            if data:
                return Path(data)
    return install_dir / "data"


def _service_lock_files(service: str, install_dir: Path, data_dir: Path) -> list[Path]:
    locks = []
    if service == "warehouse":
        locks.append(data_dir / "warehouse.lock")
    elif service == "market":
        locks.append(data_dir / "market.lock")
    elif service == "trading":
        locks.append(install_dir / ".program.lock")
        if (data_dir / "owners").is_dir():
            locks.extend(sorted((data_dir / "owners").glob("*")))
        if data_dir.is_dir():
            for child in sorted(data_dir.iterdir()):
                if not child.is_dir() or child.name == "owners":
                    continue
                locks.append(child / "owner" / "account.lock")
                locks.append(child / "strategies" / "owner")
    return [p for p in locks if p.is_file()]


def _check_sqlite_integrity(path: Path):
    wal = path.with_name(path.name + "-wal")
    if wal.is_file():
        # Copy database and WAL sidecars to a scratch directory so integrity_check
        # replays uncheckpointed WAL frames without mutating the source files.
        with tempfile.TemporaryDirectory(prefix="ast-sqlite-") as tmp:
            copy = Path(tmp) / path.name
            shutil.copy2(path, copy)
            shutil.copy2(wal, Path(tmp) / wal.name)
            shm = path.with_name(path.name + "-shm")
            if shm.is_file():
                shutil.copy2(shm, Path(tmp) / shm.name)
            with sqlite3.connect(copy) as conn:
                row = conn.execute("PRAGMA integrity_check").fetchone()
                if not row or row[0] != "ok":
                    raise RuntimeError(f"SQLite integrity check failed for {path}: {row}")
        return
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as conn:
        row = conn.execute("PRAGMA integrity_check").fetchone()
        if not row or row[0] != "ok":
            raise RuntimeError(f"SQLite integrity check failed for {path}: {row}")


def _is_under(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _collect_files(root: Path, prefix: str, exclude_lock_names: set[str]) -> list[tuple[Path, str]]:
    """Collect regular files under `root` mapped to `<prefix>/<relpath>`."""
    if not root.exists():
        return []
    items = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        for name in filenames:
            path = Path(dirpath) / name
            if path.is_symlink() or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            if name in exclude_lock_names or name.endswith(".tmp"):
                continue
            items.append((path, f"{prefix}/{rel}"))
    return items


def backup(
    service: str,
    install_dir: Path,
    output: Path,
    *,
    skip_systemd_check: bool = False,
) -> dict:
    if service not in SERVICES:
        raise ValueError(f"unsupported service: {service}")
    install_dir = install_dir.resolve()
    if not install_dir.is_dir():
        raise FileNotFoundError(f"service install directory does not exist: {install_dir}")
    _check_systemd_stopped(service, skip_systemd_check)

    data_dir = _service_data_dir(service, install_dir)
    external_data = data_dir.exists() and not _is_under(data_dir, install_dir)
    locks = _try_locks(_service_lock_files(service, install_dir, data_dir))
    try:
        entries: list[tuple[Path, str]] = []
        # Always include config/ (non-secret configs and service TLS identity).
        entries.extend(_collect_files(install_dir / "config", "install/config", set()))
        # Include data/ inside install_dir if present and not external.
        if not external_data:
            entries.extend(_collect_files(data_dir, "install/data", {"warehouse.lock", "market.lock"}))
        else:
            entries.extend(_collect_files(data_dir, "external-data", {"warehouse.lock", "market.lock"}))

        if not entries:
            raise ValueError(f"no config or data files found to back up under {install_dir}")

        # Verify SQLite databases before archiving so we never ship a corrupted backup.
        for src, _ in entries:
            if src.suffix == ".sqlite":
                _check_sqlite_integrity(src)

        output = output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        tmp_archive = output.with_name(f".{output.name}.{os.getpid()}.tmp")

        manifest_files = []
        seen_inodes: dict[tuple[int, int], str] = {}
        try:
            with tarfile.open(tmp_archive, "w:gz") as tar:
                for src, arcname in entries:
                    st = src.stat()
                    digest = _sha256_file(src)
                    inode_key = (st.st_dev, st.st_ino)
                    link_target = seen_inodes.get(inode_key)
                    if link_target is not None:
                        info = tarfile.TarInfo(name=arcname)
                        info.type = tarfile.LNKTYPE
                        info.linkname = link_target
                        info.mode = st.st_mode & 0o777
                        info.mtime = int(st.st_mtime)
                        tar.addfile(info)
                    else:
                        seen_inodes[inode_key] = arcname
                        info = tar.gettarinfo(str(src), arcname=arcname)
                        info.mode = st.st_mode & 0o777
                        with src.open("rb") as stream:
                            tar.addfile(info, stream)
                    manifest_files.append({
                        "path": arcname,
                        "size": st.st_size,
                        "sha256": digest,
                        "mode": f"{st.st_mode & 0o777:04o}",
                        **({"hardlink": link_target} if link_target is not None else {}),
                    })

                manifest = {
                    "format": 1,
                    "service": service,
                    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                    "install_dir": str(install_dir),
                    "data_dir": str(data_dir.resolve() if data_dir.exists() else data_dir),
                    "external_data": external_data,
                    "excludes_host_credentials": True,
                    "files": manifest_files,
                }
                raw_manifest = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode()
                m_info = tarfile.TarInfo(name=MANIFEST_NAME)
                m_info.size = len(raw_manifest)
                m_info.mode = 0o644
                tar.addfile(m_info, io.BytesIO(raw_manifest))

            os.replace(tmp_archive, output)
        except BaseException:
            if tmp_archive.exists():
                tmp_archive.unlink()
            raise

        archive_sha = _sha256_file(output)
        sidecar = Path(f"{output}.sha256")
        sidecar.write_text(f"{archive_sha}  {output.name}\n")
        return {
            "archive": str(output),
            "sha256": archive_sha,
            "sidecar": str(sidecar),
            "service": service,
            "file_count": len(manifest_files),
            "external_data": external_data,
        }
    finally:
        _release_locks(locks)


def _validate_archive_path(name: str):
    parts = Path(name).parts
    if not parts or name.startswith("/") or ".." in parts:
        raise ValueError(f"unsafe archive member path: {name}")


def _verify_warehouse_tree(root: Path):
    """Verify current pointer and all retained version/shard/partition digests under `root`."""
    candidates = []
    if (root / "warehouse" / "current").is_file():
        candidates.append(root / "warehouse")
    if (root / "sources").is_dir():
        for entry in sorted((root / "sources").iterdir()):
            if (entry / "warehouse" / "current").is_file():
                candidates.append(entry / "warehouse")
    for wh_root in candidates:
        current_id = (wh_root / "current").read_text().strip()
        versions_dir = wh_root / "versions"
        current_path = versions_dir / f"{current_id}.json"
        if not current_path.is_file():
            raise ValueError(f"missing current Warehouse version manifest: {current_path}")
        version_files = sorted(versions_dir.glob("*.json"))
        verified_shards: set[str] = set()
        verified_files: dict[Path, str] = {}
        for v_path in version_files:
            v_id = v_path.stem
            raw_v = v_path.read_bytes()
            if hashlib.sha256(raw_v).hexdigest() != v_id:
                raise ValueError(f"Warehouse version digest mismatch: {v_id}")
            doc = json.loads(raw_v)
            for shard_digest in doc.get("shards", {}).values():
                if shard_digest in verified_shards:
                    continue
                s_path = wh_root / "shards" / f"{shard_digest}.json"
                if not s_path.is_file():
                    raise ValueError(f"Warehouse shard missing: {s_path}")
                raw_s = s_path.read_bytes()
                if hashlib.sha256(raw_s).hexdigest() != shard_digest:
                    raise ValueError(f"Warehouse shard digest mismatch: {shard_digest}")
                for part in json.loads(raw_s).values():
                    f_path = wh_root / part["file"]
                    if not f_path.is_file():
                        raise ValueError(f"Warehouse partition file missing: {f_path}")
                    actual = verified_files.get(f_path)
                    if actual is None:
                        actual = _sha256_file(f_path)
                        verified_files[f_path] = actual
                    if actual != part["sha256"]:
                        raise ValueError(f"Warehouse partition SHA-256 mismatch: {f_path}")
                verified_shards.add(shard_digest)


def verify(archive: Path, *, expected_sha256: str = "") -> dict:
    archive = archive.resolve()
    if not archive.is_file():
        raise FileNotFoundError(f"backup archive not found: {archive}")
    actual_sha = _sha256_file(archive)
    sidecar = Path(f"{archive}.sha256")
    if expected_sha256:
        if actual_sha != expected_sha256.strip().lower():
            raise ValueError(f"archive SHA-256 mismatch: expected {expected_sha256}, got {actual_sha}")
    elif sidecar.is_file():
        listed = sidecar.read_text().strip().split()[0].lower()
        if actual_sha != listed:
            raise ValueError(f"archive SHA-256 differs from {sidecar.name}: expected {listed}, got {actual_sha}")

    with tempfile.TemporaryDirectory(prefix="ast-verify-") as tmp:
        stage = Path(tmp)
        manifest = None
        seen_members = set()
        with tarfile.open(archive, "r:gz") as tar:
            members = tar.getmembers()
            for member in members:
                _validate_archive_path(member.name)
                if member.issym() or member.isdev() or member.isfifo():
                    raise ValueError(f"unsupported archive member type: {member.name}")
                if member.name == MANIFEST_NAME:
                    f = tar.extractfile(member)
                    if f is None:
                        raise ValueError("unreadable backup-manifest.json")
                    manifest = json.loads(f.read().decode())
                    continue
                if member.islnk():
                    _validate_archive_path(member.linkname)
                    target = stage / member.linkname
                    dest = stage / member.name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    os.link(target, dest)
                    seen_members.add(member.name)
                elif member.isfile():
                    dest = stage / member.name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    src_f = tar.extractfile(member)
                    if src_f is None:
                        raise ValueError(f"unreadable archive member: {member.name}")
                    with dest.open("wb") as out_f:
                        shutil.copyfileobj(src_f, out_f)
                    seen_members.add(member.name)

        if manifest is None or manifest.get("format") != 1 or manifest.get("service") not in SERVICES:
            raise ValueError("invalid or missing backup-manifest.json")

        expected_files = {item["path"]: item for item in manifest["files"]}
        if set(expected_files) != seen_members:
            missing = sorted(set(expected_files) - seen_members)
            extra = sorted(seen_members - set(expected_files))
            raise ValueError(f"archive file list mismatch (missing={missing[:5]}, extra={extra[:5]})")

        sqlite_checked = 0
        for rel, item in expected_files.items():
            path = stage / rel
            if path.stat().st_size != item["size"]:
                raise ValueError(f"size mismatch for {rel}: expected {item['size']}, got {path.stat().st_size}")
            digest = _sha256_file(path)
            if digest != item["sha256"]:
                raise ValueError(f"SHA-256 mismatch for {rel}")
            if path.suffix == ".sqlite":
                _check_sqlite_integrity(path)
                sqlite_checked += 1

        if manifest["service"] == "warehouse":
            data_root = stage / ("external-data" if manifest.get("external_data") else "install/data")
            if data_root.is_dir():
                _verify_warehouse_tree(data_root)

        return {
            "ok": True,
            "archive": str(archive),
            "sha256": actual_sha,
            "service": manifest["service"],
            "install_dir": manifest["install_dir"],
            "data_dir": manifest["data_dir"],
            "external_data": bool(manifest.get("external_data")),
            "files_verified": len(expected_files),
            "sqlite_verified": sqlite_checked,
        }


def _resolve_owner(owner: str | None, install_dir: Path) -> tuple[int, int] | None:
    if owner:
        try:
            entry = pwd.getpwnam(owner)
        except KeyError:
            raise ValueError(f"unknown service owner user: {owner}") from None
        return entry.pw_uid, entry.pw_gid
    if os.geteuid() != 0:
        return None
    for probe in (install_dir / "config", install_dir / "logs", install_dir / "data"):
        if probe.exists():
            st = probe.stat()
            if st.st_uid != 0:
                return st.st_uid, st.st_gid
    sudo_user = os.environ.get("SUDO_USER", "")
    if sudo_user and sudo_user != "root":
        try:
            entry = pwd.getpwnam(sudo_user)
            return entry.pw_uid, entry.pw_gid
        except KeyError:
            pass
    raise ValueError(
        "restoring as root requires --owner <service-user> (or an existing non-root install directory)"
    )


def _clean_directory(directory: Path, keep_names: set[str]):
    if not directory.is_dir():
        return
    for child in sorted(directory.iterdir()):
        if child.name in keep_names:
            continue
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()


def _apply_ownership_and_dir_modes(root: Path, owner: tuple[int, int] | None):
    if not root.is_dir():
        return
    os.chmod(root, 0o700)
    if owner is not None:
        os.chown(root, owner[0], owner[1])
    for dirpath, dirnames, filenames in os.walk(root):
        base = Path(dirpath)
        for name in dirnames:
            sub = base / name
            if not sub.is_symlink():
                os.chmod(sub, 0o700)
                if owner is not None:
                    os.chown(sub, owner[0], owner[1])
        if owner is not None:
            for name in filenames:
                file_path = base / name
                if not file_path.is_symlink():
                    os.chown(file_path, owner[0], owner[1])


def _relocate_trading_owners(data_dir: Path, orig_data: Path, orig_install: Path) -> int:
    owners_dir = data_dir / "owners"
    if not owners_dir.is_dir():
        return 0
    updated = 0
    orig_bases = [orig_data]
    orig_default = orig_install / "data"
    if orig_default not in orig_bases:
        orig_bases.append(orig_default)
    for binding_path in sorted(owners_dir.glob("*.json")):
        if binding_path.is_symlink() or not binding_path.is_file():
            continue
        doc = json.loads(binding_path.read_text())
        recorded = doc.get("directory")
        if not isinstance(recorded, str) or not recorded:
            continue
        recorded_path = Path(recorded)
        rel = None
        for base in orig_bases:
            try:
                rel = recorded_path.relative_to(base)
                break
            except ValueError:
                continue
        if rel is None:
            continue
        target_dir = str((data_dir / rel).resolve())
        if target_dir == recorded:
            continue
        mode = binding_path.stat().st_mode & 0o777
        doc["directory"] = target_dir
        tmp = binding_path.with_name(f".{binding_path.name}.{os.getpid()}.tmp")
        tmp.write_text(json.dumps(doc, ensure_ascii=False))
        os.chmod(tmp, mode)
        os.replace(tmp, binding_path)
        updated += 1
    return updated


def restore(
    archive: Path,
    *,
    target_install_dir: Path | None = None,
    target_data_dir: Path | None = None,
    expected_sha256: str = "",
    force: bool = False,
    allow_relocate: bool = False,
    skip_systemd_check: bool = False,
    owner: str | None = None,
) -> dict:
    verified = verify(archive, expected_sha256=expected_sha256)
    service = verified["service"]
    orig_install = Path(verified["install_dir"])
    orig_data = Path(verified["data_dir"])
    install_dir = (target_install_dir or orig_install).resolve()

    if service == "trading" and install_dir != orig_install.resolve() and not allow_relocate:
        raise ValueError(
            f"Trading owners/ records bind canonical paths; restore to {orig_install} "
            "or pass --allow-relocate explicitly"
        )

    if verified["external_data"]:
        data_dir = (target_data_dir or orig_data).resolve()
    else:
        data_dir = (target_data_dir.resolve() if target_data_dir is not None else install_dir / "data")

    if service == "trading" and data_dir != orig_data.resolve() and not allow_relocate:
        raise ValueError(
            f"Trading owners/ records bind canonical paths; restore to {orig_data} "
            "or pass --allow-relocate explicitly"
        )

    target_owner = _resolve_owner(owner, install_dir)
    _check_systemd_stopped(service, skip_systemd_check)
    locks = _try_locks(_service_lock_files(service, install_dir, data_dir))
    try:
        if not force:
            for check_dir in (install_dir / "config", data_dir):
                if check_dir.is_dir() and any(check_dir.iterdir()):
                    raise FileExistsError(
                        f"target directory is not empty: {check_dir}; pass --force to overwrite"
                    )

        install_dir.mkdir(parents=True, exist_ok=True)
        data_dir.mkdir(parents=True, exist_ok=True)

        extracted_targets: dict[str, Path] = {}
        with tarfile.open(archive, "r:gz") as tar:
            members = tar.getmembers()
            manifest_member = next(m for m in members if m.name == MANIFEST_NAME)
            manifest = json.loads(tar.extractfile(manifest_member).read().decode())
            modes = {item["path"]: int(item["mode"], 8) for item in manifest["files"]}

            if force:
                has_config = any(item["path"].startswith("install/config/") for item in manifest["files"])
                has_data = any(
                    item["path"].startswith(("install/data/", "external-data/"))
                    for item in manifest["files"]
                )
                if has_config:
                    _clean_directory(install_dir / "config", set())
                if has_data:
                    # Purge stale SQLite -wal/-shm, old owners/*.json, and obsolete files while keeping
                    # top-level service lock inodes intact.
                    _clean_directory(data_dir, {"warehouse.lock", "market.lock"})

            def _dest_for(arcname: str) -> Path:
                if arcname.startswith("install/config/"):
                    return install_dir / "config" / arcname.removeprefix("install/config/")
                if arcname.startswith("install/data/"):
                    return data_dir / arcname.removeprefix("install/data/")
                if arcname.startswith("external-data/"):
                    return data_dir / arcname.removeprefix("external-data/")
                raise ValueError(f"unexpected archive prefix: {arcname}")

            for member in members:
                if member.name == MANIFEST_NAME:
                    continue
                dest = _dest_for(member.name)
                dest.parent.mkdir(parents=True, exist_ok=True)
                if dest.exists() or dest.is_symlink():
                    dest.unlink()
                if member.islnk():
                    os.link(extracted_targets[member.linkname], dest)
                elif member.isfile():
                    src_f = tar.extractfile(member)
                    with dest.open("wb") as out_f:
                        shutil.copyfileobj(src_f, out_f)
                    os.chmod(dest, modes.get(member.name, 0o644))
                extracted_targets[member.name] = dest

            # Verify every restored file on disk against manifest SHA-256.
            for item in manifest["files"]:
                dest = extracted_targets[item["path"]]
                if _sha256_file(dest) != item["sha256"]:
                    raise RuntimeError(f"post-restore SHA-256 mismatch on {dest}")

        # Update config "data" path if data_dir changed on Warehouse or Market.
        config_updated = False
        if service in ("warehouse", "market"):
            cfg_path = install_dir / "config" / f"{service}.json"
            if cfg_path.is_file():
                cfg = json.loads(cfg_path.read_text())
                if cfg.get("data") != str(data_dir):
                    mode = cfg_path.stat().st_mode & 0o777
                    cfg["data"] = str(data_dir)
                    cfg_path.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")
                    os.chmod(cfg_path, mode)
                    config_updated = True

        owners_updated = 0
        if service == "trading" and (data_dir != orig_data or install_dir != orig_install):
            owners_updated = _relocate_trading_owners(data_dir, orig_data, orig_install)

        _apply_ownership_and_dir_modes(install_dir / "config", target_owner)
        _apply_ownership_and_dir_modes(data_dir, target_owner)

        return {
            "ok": True,
            "service": service,
            "install_dir": str(install_dir),
            "data_dir": str(data_dir),
            "files_restored": len(extracted_targets),
            "config_data_updated": config_updated,
            "owners_updated": owners_updated,
            "owner": f"{target_owner[0]}:{target_owner[1]}" if target_owner is not None else None,
            "next_steps": [
                "在新主机重新录入 systemd 加密凭据（冷备不含原主机加密凭据）",
                "核对服务端 TLS 证书指纹与配置后启动服务",
                *(["Trading 启动后核对持仓、未确认委托与柜台事实，再显式允许下单"]
                  if service == "trading" else []),
            ],
        }
    finally:
        _release_locks(locks)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Asterion Market/Warehouse/Trading cold backup & restore")
    sub = parser.add_subparsers(dest="command", required=True)

    p_bak = sub.add_parser("backup", help="Create a verified cold backup archive")
    p_bak.add_argument("--service", required=True, choices=SERVICES)
    p_bak.add_argument("--install-dir", type=Path, default=None)
    p_bak.add_argument("--output", required=True, type=Path)
    p_bak.add_argument("--skip-systemd-check", action="store_true")

    p_ver = sub.add_parser("verify", help="Verify archive SHA-256, manifest, SQLite, and Warehouse integrity")
    p_ver.add_argument("--archive", required=True, type=Path)
    p_ver.add_argument("--sha256", default="")

    p_res = sub.add_parser("restore", help="Verify and restore a cold backup archive")
    p_res.add_argument("--archive", required=True, type=Path)
    p_res.add_argument("--target-install-dir", type=Path, default=None)
    p_res.add_argument("--target-data-dir", type=Path, default=None)
    p_res.add_argument("--sha256", default="")
    p_res.add_argument("--force", action="store_true")
    p_res.add_argument("--allow-relocate", action="store_true")
    p_res.add_argument("--skip-systemd-check", action="store_true")
    p_res.add_argument("--owner", default=None, help="Linux service user to own restored config/ and data/")

    args = parser.parse_args(argv)
    try:
        if args.command == "backup":
            install_dir = args.install_dir or DEFAULT_INSTALL_DIRS[args.service]
            result = backup(args.service, install_dir, args.output, skip_systemd_check=args.skip_systemd_check)
        elif args.command == "verify":
            result = verify(args.archive, expected_sha256=args.sha256)
        else:
            result = restore(
                args.archive,
                target_install_dir=args.target_install_dir,
                target_data_dir=args.target_data_dir,
                expected_sha256=args.sha256,
                force=args.force,
                allow_relocate=args.allow_relocate,
                skip_systemd_check=args.skip_systemd_check,
                owner=args.owner,
            )
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
