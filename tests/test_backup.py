from configuration_support import set_token
from credential_helpers import provider_secrets
from import_identity_support import import_identity
from rules_support import rule_access
from storage_support import data_store, domain_tasks, raw_engine, research_store, scheduler

from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.platform.tasks.execution import ExecutionContext
from asterion.research.strategies import STRATEGY_RESOURCE

"""Backups preserve private state and reject overwrite, corruption and unsafe extraction."""

import fcntl
import json
import stat
import zipfile

import pytest
from sqlalchemy import create_engine
from test_research import config

from asterion.api.app import create_app
from asterion.data.importing import encode_import
from asterion.data.library import DataLibrary
from asterion.data.lifecycle import ArchiveRequest, VersionLifecycle
from asterion.data.public import ImportOptions, ImportRequest
from asterion.data.snapshots import Snapshots
from asterion.data.sync import DataSync
from asterion.platform.store import metadata
from asterion.research.service import Backtests
from asterion.research.worker import execute
from asterion.runtime.backup import create_backup, restore_backup, unpack
from asterion.runtime.desktop import initialize_postgres, load_config, pg_command, runtime_settings


@pytest.fixture
def state(tmp_path):
    state = tmp_path / "state"
    load_config(state)
    (state / "postgres/empty").mkdir(parents=True)
    (state / "postgres/PG_VERSION").write_text("17")
    (state / "data/.credentials").mkdir(parents=True)
    (state / "data/.credentials/test.enc").write_bytes(b"secret-fixture")
    return state


def test_snapshot_private_and_checksum_roundtrip(state, tmp_path):
    path = tmp_path / "backup.zip"
    result = create_backup(state, path)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert result["files"] == 3
    target = tmp_path / "restored"
    target.mkdir()
    unpack(path, target)
    assert (target / "postgres/empty").is_dir()
    assert (target / "desktop.json").read_bytes() == (state / "desktop.json").read_bytes()
    assert (target / "data/.credentials/test.enc").read_bytes() == b"secret-fixture"
    with pytest.raises(ValueError, match="存在"):
        create_backup(state, path)
    with pytest.raises(ValueError, match="之外"):
        create_backup(state, state / "nested.zip")
    with pytest.raises(ValueError, match="新目录"):
        restore_backup(path, state, tmp_path)


def test_active_service_pid_and_symlink_refuse_backup(state, tmp_path):
    with (state / "supervisor.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with pytest.raises(ValueError, match="运行"):
            create_backup(state, tmp_path / "busy.zip")
    (state / "postgres/postmaster.pid").write_text("123")
    with pytest.raises(ValueError, match="数据库尚未停止"):
        create_backup(state, tmp_path / "pid.zip")
    (state / "postgres/postmaster.pid").unlink()
    (state / "data/link").symlink_to(state / "desktop.json")
    with pytest.raises(ValueError, match="链接"):
        create_backup(state, tmp_path / "link.zip")
    assert not (tmp_path / "link.zip").exists()


@pytest.mark.parametrize("damage", ["checksum", "traversal", "extra", "platform", "duplicate"])
def test_restore_rejects_invalid_archives_without_publishing_target(state, tmp_path, damage):
    path = tmp_path / "original.zip"
    create_backup(state, path)
    with zipfile.ZipFile(path) as source:
        files = {name: source.read(name) for name in source.namelist()}
    manifest = json.loads(files["manifest.json"])
    if damage == "checksum":
        files["data/.credentials/test.enc"] = b"broken-fixture"
    elif damage == "traversal":
        manifest["directories"].append("data/../../outside")
    elif damage == "extra":
        files["extra"] = b"unexpected"
    elif damage == "platform":
        manifest["platform"] = "unsupported"
    files["manifest.json"] = json.dumps(manifest).encode()
    broken = tmp_path / "broken.zip"
    with zipfile.ZipFile(broken, "w") as archive:
        for name, content in files.items():
            archive.writestr(name, content)
        if damage == "duplicate":
            with pytest.warns(UserWarning):
                archive.writestr("desktop.json", files["desktop.json"])
    target = tmp_path / "restored"
    with pytest.raises(ValueError):
        restore_backup(broken, target, tmp_path)
    assert not target.exists()
    assert not (tmp_path / "outside").exists()


def test_real_postgres_restore_research_archive_credentials_and_queue_isolation(
    tmp_path, system_postgres
):
    pg = system_postgres
    state = tmp_path / "original"
    settings = load_config(state)
    initialize_postgres(state, pg, settings)
    engine = create_engine(runtime_settings(state, settings).database_url)
    try:
        metadata.create_all(engine)
        create_app(runtime_settings(state, settings), raw_engine(engine))
        tasks = scheduler(engine)
        sync = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            state / "data",
            provider_secrets(settings["token"]),
        )
        set_token(sync, "tushare", "offline-test-credential")
        request = ImportRequest(
            command_id="fixture-import",
            source="backup-test",
            csv="contract,trading_day,open,high,low,close,vol,settle\n"
            + "\n".join(f"SHFE.rb2405,2024-01-0{d},10,12,9,11,100,10.5" for d in (2, 3, 4)),
            options=ImportOptions(
                identity=import_identity("SHFE.rb2405"),
                type_id="futures.daily",
                frequency="1d",
                source_id="backup",
            ),
        )
        tasks.submit(
            request.command_id,
            "data.import_csv",
            request.model_dump(mode="json", exclude={"command_id"}),
        )
        claimed = tasks.claim("import")
        Snapshots(
            data_store(engine), domain_tasks(data_store(engine), "data"), state / "data"
        ).publish(claimed["id"], claimed["token"], encode_import(claimed["payload"])[0])
        daily = sync.library.list(type_id="futures.daily", layer="STANDARD")["items"][0]
        research = Backtests(
            research_store(engine),
            domain_tasks(research_store(engine), "research"),
            version_access(engine, state / "data"),
            rule_access(engine),
            strategy_catalog(),
        )
        from reference_support import research_coverage

        report = research_coverage(
            data_store(engine), state / "data", daily["id"], "2024-01-01", "2024-01-10"
        )
        job = research.submit(
            config().model_copy(
                update={"version_id": daily["id"], "coverage_report_id": report["id"]}
            )
        )
        claimed = tasks.claim("research")
        research.publish(
            job["id"],
            claimed["token"],
            execute(
                ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
                claimed["payload"],
            )[0],
        )
        VersionLifecycle(data_store(engine)).archive(
            daily["id"], ArchiveRequest(archived=True, expected_revision=0)
        )
        tasks.submit("never-run-automatically", "data.sync", {"provider": "tushare"})
        expected = research.get(job["id"])["output"]
    finally:
        engine.dispose()
        pg_command(pg, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop")
    archive = tmp_path / "full.zip"
    create_backup(state, archive)
    target = tmp_path / "recovered"
    report = restore_backup(archive, target, pg)
    assert report["research_results"] == 1 and report["quarantined_tasks"] == 1
    assert report["versions"] == 6
    restored_settings = json.loads((target / "desktop.json").read_text())
    assert restored_settings["db_port"] != settings["db_port"]
    initialize_postgres(target, pg, restored_settings)
    engine = create_engine(runtime_settings(target, restored_settings).database_url)
    try:
        tasks = scheduler(engine)
        research = Backtests(
            research_store(engine),
            domain_tasks(research_store(engine), "research"),
            version_access(engine, target / "data"),
            rule_access(engine),
            strategy_catalog(),
        )
        assert research.get(job["id"])["output"] == expected
        assert DataLibrary(data_store(engine), target / "data").preview(daily["id"])["total"] == 3
        assert VersionLifecycle(data_store(engine)).inspect(daily["id"])["archived"]
        sync = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            target / "data",
            provider_secrets(restored_settings["token"]),
        )
        assert sync.configuration.current("tushare")[1] == {"token": "offline-test-credential"}
        assert tasks.claim("must-not-resume") is None
        research.rerun(job["id"], "explicit-recovered-replay")
        claimed = tasks.claim("explicit-replay")
        assert (
            json.loads(
                execute(
                    ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
                    claimed["payload"],
                )[0]
            )
            == expected
        )
    finally:
        engine.dispose()
        pg_command(pg, "pg_ctl", "-D", str(target / "postgres"), "-m", "fast", "-w", "stop")


def test_managed_failure_restores_previously_running_service(state, tmp_path, monkeypatch):
    from asterion.runtime import backup

    calls = []
    monkeypatch.setattr(backup, "healthy", lambda _: True)
    monkeypatch.setattr(backup, "stop", lambda _: calls.append("stop"))
    monkeypatch.setattr(backup, "bootstrap", lambda *_: calls.append("start"))

    def failed(*_):
        raise ValueError("fixture-copy-failure")

    monkeypatch.setattr(backup, "create_backup", failed)
    with pytest.raises(ValueError, match="fixture-copy-failure"):
        backup.managed_backup(state, tmp_path, tmp_path / "failure.zip")
    assert calls == ["stop", "start"]
    calls.clear()
    monkeypatch.setattr(backup, "healthy", lambda _: False)
    with pytest.raises(ValueError, match="fixture-copy-failure"):
        backup.managed_backup(state, tmp_path, tmp_path / "failure.zip")
    assert calls == []


def version_access(engine, root):
    reader = VersionReader(data_store(engine), root)
    return VersionAccess(reader.read, reader.coverage)
