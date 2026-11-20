"""Cold backup, verification, and restore drill tests for Market, Warehouse, and Trading."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "services"))
sys.path.insert(0, str(ROOT / "tests" / "lab"))
sys.path.insert(0, str(ROOT / "apps" / "services" / "lab"))

import cold_backup  # noqa: E402


def _make_sqlite(path: Path, table_sql: str, insert_sql: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as conn:
        conn.execute(table_sql)
        conn.execute(insert_sql)


def _make_warehouse_store(wh_root: Path, provider: str, content: bytes):
    import hashlib

    (wh_root / "files" / "daily" / "SHFE").mkdir(parents=True, exist_ok=True)
    (wh_root / "shards").mkdir(parents=True, exist_ok=True)
    (wh_root / "versions").mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(content).hexdigest()
    rel = f"files/daily/SHFE/2026-01-{digest[:16]}.parquet"
    f_path = wh_root / rel
    f_path.write_bytes(content)
    shard = {"daily/SHFE/2026-01": {"file": rel, "sha256": digest, "rows": 1, "days": ["2026-01-05"]}}
    raw_shard = json.dumps(shard, sort_keys=True, separators=(",", ":")).encode()
    s_digest = hashlib.sha256(raw_shard).hexdigest()
    (wh_root / "shards" / f"{s_digest}.json").write_bytes(raw_shard)
    manifest = {"format": 2, "provider": provider, "parent": None, "created_ns": 1, "shards": {"daily/SHFE/2026-01": s_digest}}
    raw_v = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    v_digest = hashlib.sha256(raw_v).hexdigest()
    (wh_root / "versions" / f"{v_digest}.json").write_bytes(raw_v)
    (wh_root / "current").write_text(v_digest)
    return f_path, rel, digest, s_digest, v_digest


class ColdBackupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_warehouse_external_data_hardlinks_backup_verify_restore_and_tamper(self):
        install = self.root / "wh-install"
        ext_data = self.root / "wh-disk"
        (install / "config" / "tls").mkdir(parents=True)
        (install / "config" / "warehouse.json").write_text(json.dumps({"data": str(ext_data), "port": 7450}))
        key_path = install / "config" / "tls" / "server.key"
        key_path.write_text("SECRET-KEY")
        os.chmod(key_path, 0o600)

        t_file, rel, digest, _, _ = _make_warehouse_store(ext_data / "warehouse", "tushare", b"PARQUET-BYTES-1")
        # Research hard-links the same Parquet file.
        r_root = ext_data / "sources" / "research" / "warehouse"
        (r_root / rel).parent.mkdir(parents=True, exist_ok=True)
        os.link(t_file, r_root / rel)
        import hashlib
        shard = {"daily/SHFE/2026-01": {"file": rel, "sha256": digest, "rows": 1, "days": ["2026-01-05"], "source": "tushare"}}
        raw_s = json.dumps(shard, sort_keys=True, separators=(",", ":")).encode()
        s_dig = hashlib.sha256(raw_s).hexdigest()
        (r_root / "shards").mkdir(parents=True, exist_ok=True)
        (r_root / "shards" / f"{s_dig}.json").write_bytes(raw_s)
        raw_v = json.dumps({"format": 2, "provider": "research", "parent": None, "created_ns": 1, "shards": {"daily/SHFE/2026-01": s_dig}}, sort_keys=True, separators=(",", ":")).encode()
        v_dig = hashlib.sha256(raw_v).hexdigest()
        (r_root / "versions").mkdir(parents=True, exist_ok=True)
        (r_root / "versions" / f"{v_dig}.json").write_bytes(raw_v)
        (r_root / "current").write_text(v_dig)

        # Active warehouse.lock blocks cold backup.
        lock_file = ext_data / "warehouse.lock"
        lock_file.write_text("")
        fd = os.open(lock_file, os.O_RDWR)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            with self.assertRaisesRegex(RuntimeError, "lock is held"):
                cold_backup.backup("warehouse", install, self.root / "wh.tar.gz", skip_systemd_check=True)
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

        archive = self.root / "wh.tar.gz"
        backed = cold_backup.backup("warehouse", install, archive, skip_systemd_check=True)
        self.assertTrue(backed["external_data"])
        verified = cold_backup.verify(archive)
        self.assertTrue(verified["ok"])

        # Restore to a new host install & relocated data directory.
        new_install = self.root / "new-wh"
        new_data = self.root / "new-wh-disk"
        restored = cold_backup.restore(
            archive,
            target_install_dir=new_install,
            target_data_dir=new_data,
            skip_systemd_check=True,
        )
        self.assertTrue(restored["ok"])
        self.assertTrue(restored["config_data_updated"])
        self.assertEqual(json.loads((new_install / "config" / "warehouse.json").read_text())["data"], str(new_data.resolve()))
        self.assertEqual((new_install / "config" / "tls" / "server.key").stat().st_mode & 0o777, 0o600)
        # Hard link preserved across backup/restore (same inode!).
        self.assertEqual(
            (new_data / "warehouse" / rel).stat().st_ino,
            (new_data / "sources" / "research" / "warehouse" / rel).stat().st_ino,
        )

        # Tampering with archive bytes fails sidecar SHA-256 check immediately.
        archive.write_bytes(archive.read_bytes()[:-4] + b"XXXX")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            cold_backup.verify(archive)

        # Rewriting a file inside an archive without updating manifest fails per-file SHA-256 check.
        tampered = self.root / "tampered.tar.gz"
        with tarfile.open(tampered, "w:gz") as tar:
            for arc, data in (
                ("install/config/warehouse.json", b'{"port":9999}'),
                ("backup-manifest.json", json.dumps({
                    "format": 1,
                    "service": "warehouse",
                    "install_dir": str(install),
                    "data_dir": str(ext_data),
                    "external_data": False,
                    "files": [{"path": "install/config/warehouse.json", "size": 13, "sha256": "0" * 64, "mode": "0644"}],
                }).encode()),
            ):
                info = tarfile.TarInfo(name=arc)
                info.size = len(data)
                tar.addfile(info, cold_backup.io.BytesIO(data))
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            cold_backup.verify(tampered)

    def test_trading_backup_verify_restore_enforces_path_and_sqlite_integrity(self):
        install = self.root / "trading-install"
        (install / "config" / "accounts").mkdir(parents=True)
        (install / "config" / "tls").mkdir(parents=True)
        (install / "config" / "accounts" / "sim.json").write_text(json.dumps({"port": 7460}))
        key = install / "config" / "tls" / "server.key"
        key.write_text("TRADING-KEY")
        os.chmod(key, 0o600)
        (install / "data" / "owners").mkdir(parents=True)
        owner_key = hashlib.sha256(b"9999\n000001").hexdigest()
        (install / "data" / "owners" / owner_key).write_text("")
        binding_path = install / "data" / "owners" / f"{owner_key}.json"
        binding_path.write_text(json.dumps({"directory": str((install / "data" / "sim").resolve()), "initialized": True}))
        os.chmod(binding_path, 0o600)
        (install / ".program.lock").write_text("")
        # Create a real WAL-mode database and leave uncheckpointed WAL frames on disk.
        journal_path = install / "data" / "sim" / "journal.sqlite"
        journal_path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(journal_path)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA wal_autocheckpoint=0")
        conn.execute("CREATE TABLE t(x INT)")
        conn.execute("INSERT INTO t VALUES (1)")
        conn.commit()
        conn.execute("INSERT INTO t VALUES (42)")
        conn.commit()
        wal_path = journal_path.with_name("journal.sqlite-wal")
        self.assertTrue(wal_path.is_file() and wal_path.stat().st_size > 0)
        wal_bytes = wal_path.read_bytes()
        conn.close()
        # Simulate crash leaving uncheckpointed WAL beside main db.
        if not wal_path.is_file():
            wal_path.write_bytes(wal_bytes)
        _make_sqlite(install / "data" / "sim" / "archives" / "00000001.sqlite", "CREATE TABLE t(x INT)", "INSERT INTO t VALUES (2)")
        _make_sqlite(install / "data" / "sim" / "strategies" / "bars.sqlite", "CREATE TABLE b(x INT)", "INSERT INTO b VALUES (3)")
        _make_sqlite(install / "data" / "sim" / "strategies" / "runs" / "r1.sqlite", "CREATE TABLE s(x INT)", "INSERT INTO s VALUES (4)")

        # Shared .program.lock held by a running Trading instance blocks backup.
        fd = os.open(install / ".program.lock", os.O_RDONLY)
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        try:
            with self.assertRaisesRegex(RuntimeError, "lock is held"):
                cold_backup.backup("trading", install, self.root / "trading.tar.gz", skip_systemd_check=True)
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

        archive = self.root / "trading.tar.gz"
        cold_backup.backup("trading", install, archive, skip_systemd_check=True)
        ver = cold_backup.verify(archive)
        self.assertEqual(ver["sqlite_verified"], 4)

        # Relocating Trading without --allow-relocate is refused.
        other = self.root / "other-trading"
        with self.assertRaisesRegex(ValueError, "allow-relocate"):
            cold_backup.restore(archive, target_install_dir=other, skip_systemd_check=True)

        # With --allow-relocate restores cleanly, rewrites owners/*.json canonical directory, and sets 0700 dirs.
        res = cold_backup.restore(archive, target_install_dir=other, allow_relocate=True, skip_systemd_check=True)
        self.assertTrue(res["ok"])
        self.assertEqual(res["owners_updated"], 1)
        relocated_binding = json.loads((other / "data" / "owners" / f"{owner_key}.json").read_text())
        self.assertEqual(relocated_binding["directory"], str((other / "data" / "sim").resolve()))
        self.assertEqual((other / "config" / "tls" / "server.key").stat().st_mode & 0o777, 0o600)
        self.assertEqual((other / "config").stat().st_mode & 0o777, 0o700)
        self.assertEqual((other / "data").stat().st_mode & 0o777, 0o700)
        with sqlite3.connect(other / "data" / "sim" / "journal.sqlite") as restored_db:
            rows = [r[0] for r in restored_db.execute("SELECT x FROM t ORDER BY x")]
            self.assertEqual(rows, [1, 42])

        # Second restore without --force refuses overwrite; --force purges stale -wal/-shm and extra files.
        with self.assertRaises(FileExistsError):
            cold_backup.restore(archive, target_install_dir=other, allow_relocate=True, skip_systemd_check=True)
        stale_wal = other / "data" / "sim" / "archives" / "00000001.sqlite-wal"
        stale_wal.write_bytes(b"CORRUPT-WAL-FRAME")
        stale_extra = other / "config" / "accounts" / "obsolete.json"
        stale_extra.write_text("{}")
        res_force = cold_backup.restore(
            archive, target_install_dir=other, allow_relocate=True, force=True, skip_systemd_check=True
        )
        self.assertTrue(res_force["ok"])
        self.assertFalse(stale_wal.exists())
        self.assertFalse(stale_extra.exists())

    def test_verify_checks_all_retained_warehouse_versions_and_root_owner_resolution(self):
        install = self.root / "wh-versions"
        data = install / "data"
        (install / "config").mkdir(parents=True)
        (install / "config" / "warehouse.json").write_text(json.dumps({"data": str(data), "port": 7450}))
        # Create an older retained version v1 and a newer current version v2.
        f1, _, _, _, _ = _make_warehouse_store(data / "warehouse", "tushare", b"OLD-V1-PARQUET")
        _make_warehouse_store(data / "warehouse", "tushare", b"NEW-V2-PARQUET")
        # Corrupt the older retained version's parquet file while current v2 stays intact.
        f1.write_bytes(b"CORRUPTED-V1!!")
        archive = self.root / "wh-versions.tar.gz"
        cold_backup.backup("warehouse", install, archive, skip_systemd_check=True)
        with self.assertRaisesRegex(ValueError, "Warehouse partition SHA-256 mismatch"):
            cold_backup.verify(archive)

        # Running as root without --owner or non-root target/SUDO_USER fails fast; explicit --owner chowns.
        with mock.patch("os.geteuid", return_value=0), mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "--owner"):
                cold_backup._resolve_owner(None, self.root / "empty-install")
            resolved = cold_backup._resolve_owner("root", self.root / "empty-install")
            self.assertEqual(resolved, (0, 0))

    def test_market_backup_and_restore(self):
        install = self.root / "market-install"
        (install / "config" / "tls").mkdir(parents=True)
        (install / "data").mkdir(parents=True)
        (install / "config" / "market.json").write_text(json.dumps({"data": str(install / "data"), "port": 7451}))
        (install / "config" / "tls" / "server.crt").write_text("CERT")
        (install / "data" / "catalog.pb").write_bytes(b"CATALOG")
        archive = self.root / "market.tar.gz"
        cold_backup.backup("market", install, archive, skip_systemd_check=True)
        target = self.root / "market-restored"
        res = cold_backup.restore(archive, target_install_dir=target, skip_systemd_check=True)
        self.assertTrue(res["ok"])
        self.assertEqual((target / "data" / "catalog.pb").read_bytes(), b"CATALOG")
        # Also verify archive does not contain arbitrary symlinks / path traversal.
        bad = self.root / "traversal.tar.gz"
        with tarfile.open(bad, "w:gz") as tar:
            info = tarfile.TarInfo(name="../escape.txt")
            info.size = 1
            tar.addfile(info, cold_backup.io.BytesIO(b"x"))
        with self.assertRaisesRegex(ValueError, "unsafe archive member path"):
            cold_backup.verify(bad)


if __name__ == "__main__":
    unittest.main()
