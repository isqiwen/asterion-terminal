from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.tasks import ExecutionContext
from configuration_support import saved_values, set_token
from credential_helpers import provider_secrets
from import_identity_support import import_identity
from rules_support import rule_access
from storage_support import data_store, domain_tasks, raw_engine, research_store, scheduler

from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.research.execution import EXECUTION
from asterion.research.strategies import STRATEGY_RESOURCE

"""Backups preserve private state and reject overwrite, corruption and unsafe extraction."""

import fcntl
import json
import shutil
import stat
import subprocess
import zipfile

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.file_archives import FileArchiveReader, StagedDirectory
from entry_support import entry_lifecycle, start_entry_once
from import_support import import_options, import_payload, publish_import
from test_research import config

from asterion.api.app import create_app
from asterion.data.library import DataLibrary
from asterion.data.sync import DataSync
from asterion.platform.store import metadata
from asterion.research.service import Backtests
from asterion.research.worker import execute
from asterion.runtime.backup import LIMITS, MAX_FILES, create_backup, restore_backup, unpack
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
    with (
        FileArchiveReader(path, limits=LIMITS, metadata_name="manifest.json") as reader,
        StagedDirectory(target, max_entries=MAX_FILES * 2 + 1) as stage,
    ):
        unpack(reader, stage)
        assert not target.exists()
        stage.commit()
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
    with pytest.raises(ValueError, match="link"):
        create_backup(state, tmp_path / "link.zip")
    assert not (tmp_path / "link.zip").exists()


@pytest.mark.parametrize(
    "damage",
    [
        "checksum",
        "traversal",
        "extra",
        "platform",
        "duplicate",
        "missing_directories",
        "unknown_field",
        "invalid_version",
        "boolean_version",
        "active_database",
    ],
)
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
    elif damage == "missing_directories":
        del manifest["directories"]
    elif damage == "unknown_field":
        manifest["unknown"] = "unsupported"
    elif damage == "invalid_version":
        manifest["schema_version"] = 0
    elif damage == "boolean_version":
        manifest["schema_version"] = True
    elif damage == "active_database":
        files["postgres/postmaster.pid"] = b"1\n"
        from hashlib import sha256

        manifest["files"]["postgres/postmaster.pid"] = {
            "bytes": 2,
            "sha256": sha256(b"1\n").hexdigest(),
        }
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


def test_backup_metadata_budget_matches_reader_and_preserves_source(state, tmp_path, monkeypatch):
    from dataclasses import replace

    from asterion.runtime import backup

    before = (state / "desktop.json").read_bytes()
    original = backup.canonical
    monkeypatch.setattr(backup, "LIMITS", replace(LIMITS, metadata_bytes=2048))
    monkeypatch.setattr(backup, "canonical", lambda value: original(value) + b" " * 2048)
    with pytest.raises(ValueError, match="metadata exceeds"):
        create_backup(state, tmp_path / "oversized.zip")
    assert not (tmp_path / "oversized.zip").exists()
    assert not list(tmp_path.glob(".asterion-archive-*"))
    assert (state / "desktop.json").read_bytes() == before


@pytest.mark.parametrize("failure", ["verification", "concurrent_target", "after_publication"])
def test_restore_failure_retains_existing_or_published_data(state, tmp_path, monkeypatch, failure):
    from asterion.runtime import backup

    archive, target = tmp_path / "snapshot.zip", tmp_path / "restored"
    create_backup(state, archive)
    archived_bytes = archive.read_bytes()
    expected_config = (state / "desktop.json").read_bytes()

    def verify(staged, _pg, *, quarantine):
        assert quarantine and not target.exists()
        assert (staged / "desktop.json").read_bytes() == expected_config
        if failure == "verification":
            raise RuntimeError("verification failed")
        if failure == "concurrent_target":
            target.mkdir()
            (target / "existing").write_bytes(b"keep concurrent data")
        return {"quarantined_tasks": 0}

    monkeypatch.setattr(backup, "validate_database", verify)
    if failure == "after_publication":
        original = StagedDirectory.commit

        def commit(stage):
            original(stage)
            raise RuntimeError("failure after publication")

        monkeypatch.setattr(StagedDirectory, "commit", commit)
    with pytest.raises((RuntimeError, FileExistsError)):
        restore_backup(archive, target, tmp_path)
    assert not list(tmp_path.glob(".asterion-stage-*"))
    assert archive.read_bytes() == archived_bytes
    assert (state / "desktop.json").read_bytes() == expected_config
    if failure == "verification":
        assert not target.exists()
    elif failure == "concurrent_target":
        assert (target / "existing").read_bytes() == b"keep concurrent data"
        assert not (target / "desktop.json").exists()
    else:
        assert (target / "desktop.json").read_bytes() == expected_config
        assert json.loads((target / "restore-report.json").read_bytes())["status"] == "verified"


def test_backup_excludes_nested_backup_files_but_preserves_empty_directories(state, tmp_path):
    nested = state / "data/backups/retained-empty"
    nested.mkdir(parents=True)
    (state / "data/backups/old.zip").write_bytes(b"local archive")
    path = tmp_path / "snapshot.zip"
    create_backup(state, path)
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        assert "data/backups/old.zip" not in manifest["files"]
        assert "data/backups/retained-empty" in manifest["directories"]
    assert (state / "data/backups/old.zip").read_bytes() == b"local archive"


@pytest.mark.parametrize("failure", ["running", "status_error", "timeout", "interrupt"])
def test_uncertain_restore_database_shutdown_preserves_private_scene(
    state, tmp_path, monkeypatch, failure
):
    from asterion.runtime import backup

    archive, target = tmp_path / "snapshot.zip", tmp_path / "restored"
    create_backup(state, archive)
    monkeypatch.setattr(backup, "pg_directory", lambda *_: tmp_path)
    stops = []

    def stop(*args):
        stops.append(args[-3])
        raise subprocess.TimeoutExpired("fixture pg_ctl", 10)

    def status(*_args, **_kwargs):
        if failure == "timeout":
            raise subprocess.TimeoutExpired("fixture pg_ctl", 10)
        if failure == "interrupt":
            raise KeyboardInterrupt
        return subprocess.CompletedProcess([], 0 if failure == "running" else 4)

    monkeypatch.setattr(backup, "pg_command", stop)
    monkeypatch.setattr(backup.subprocess, "run", status)

    def verify(staged, pg, *, quarantine):
        assert quarantine
        backup.stop_restore_database(staged, pg)

    monkeypatch.setattr(backup, "validate_database", verify)
    with pytest.raises(backup.RestoreDatabaseUncertain, match="已保留恢复现场") as error:
        restore_backup(archive, target, tmp_path)
    retained = list(tmp_path.glob(".asterion-stage-*"))
    assert len(retained) == 1 and str(retained[0]) in str(error.value)
    assert not target.exists()
    assert (retained[0] / "desktop.json").read_bytes() == (state / "desktop.json").read_bytes()
    assert stat.S_IMODE(retained[0].stat().st_mode) == 0o700
    assert stops == (["fast", "immediate"] if failure == "running" else [])


def test_stop_failure_with_confirmed_database_exit_allows_cleanup(tmp_path, monkeypatch):
    from asterion.runtime import backup

    monkeypatch.setattr(backup, "pg_directory", lambda *_: tmp_path)
    status = iter([0, 3])
    monkeypatch.setattr(
        backup.subprocess,
        "run",
        lambda *_args, **_kwargs: subprocess.CompletedProcess([], next(status)),
    )

    def stop(*_):
        raise subprocess.CalledProcessError(1, "fixture pg_ctl")

    monkeypatch.setattr(backup, "pg_command", stop)
    backup.stop_restore_database(tmp_path, tmp_path)


def test_preserved_stage_is_closed_and_cannot_be_published_or_extracted(state, tmp_path):
    archive = tmp_path / "snapshot.zip"
    create_backup(state, archive)
    target = tmp_path / "restored"
    with (
        FileArchiveReader(archive, limits=LIMITS, metadata_name="manifest.json") as reader,
        StagedDirectory(target, max_entries=MAX_FILES * 2 + 1) as stage,
    ):
        manifest = unpack(reader, stage)
        retained = stage.preserve()
        with pytest.raises(ValueError, match="closed"):
            stage.commit()
        with pytest.raises(ValueError, match="closed"):
            reader.extract(stage, manifest.model_dump()["files"], manifest.directories)
    assert retained.is_dir() and not target.exists()


def test_restore_resolves_host_parent_alias_but_rejects_target_links(state, tmp_path, monkeypatch):
    from asterion.runtime import backup

    archive = tmp_path / "snapshot.zip"
    create_backup(state, archive)
    parent = tmp_path / "owned"
    parent.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(parent, target_is_directory=True)
    monkeypatch.setattr(
        backup, "validate_database", lambda *_args, **_kwargs: {"quarantined_tasks": 0}
    )
    report = restore_backup(archive, alias / "restored", tmp_path)
    assert report["target"] == str(parent / "restored")
    assert (parent / "restored/desktop.json").read_bytes() == (state / "desktop.json").read_bytes()
    (alias / "leaf").symlink_to(parent / "missing", target_is_directory=True)
    with pytest.raises(ValueError, match="新目录"):
        restore_backup(archive, alias / "leaf", tmp_path)
    assert not (parent / "missing").exists()


def test_real_postgres_restore_research_archive_credentials_and_queue_isolation(
    tmp_path, system_postgres, monkeypatch
):
    pg = system_postgres
    state = tmp_path / "original"
    settings = load_config(state)
    initialize_postgres(state, pg, settings)
    engine = create_engine(runtime_settings(state, settings).database_url)
    try:
        metadata.create_all(engine)
        create_app(runtime_settings(state, settings), raw_engine(engine))
        start_entry_once(runtime_settings(state, settings))
        tasks = scheduler(engine)
        sync = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            state / "data",
            provider_secrets(settings["token"], state / "data"),
        )
        set_token(sync, "tushare", "offline-test-credential")
        csv = "contract,trading_day,open,high,low,close,vol,settle\n" + "\n".join(
            f"SHFE.rb2405,2024-01-0{d},10,12,9,11,100,10.5" for d in (2, 3, 4)
        )
        options = import_options(
            import_identity("SHFE.rb2405"),
            type_id="futures.daily",
            frequency="1d",
            source_id="backup",
        )
        tasks.submit(
            "fixture-import", "data.import_csv", import_payload(csv, options, "backup-test")
        )
        claimed = tasks.claim("import")
        publish_import(engine, state / "data", claimed)
        daily = sync.library.list(type_id="futures.daily", layer="STANDARD")["items"][0]
        research = Backtests(
            research_store(engine),
            domain_tasks(research_store(engine), "research"),
            version_access(engine, state / "data"),
            rule_access(engine),
            strategy_catalog(),
            ExecutionFactory(),
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
                ExecutionContext(
                    (
                        STRATEGY_RESOURCE,
                        EXECUTION,
                    ),
                    {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
                ),
                claimed["payload"],
            )[0],
        )
        with entry_lifecycle(
            runtime_settings(state, settings).database_url, state / "data"
        ) as life:
            life.archive(daily["id"], archived=True, expected_revision=0)
        tasks.submit("never-run-automatically", "data.sync", {"provider": "tushare"})
        expected = research.get(job["id"])["output"]
    finally:
        engine.dispose()
        pg_command(pg, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop")
    external = tmp_path / "external-database"
    external.mkdir()
    (external / "untouched").write_bytes(b"never use or modify this directory")
    (state / "postgres/redirect.conf").write_text(f"data_directory = '{external}'\n")
    with (state / "postgres/postgresql.conf").open("a") as config_file:
        config_file.write("\ninclude = 'redirect.conf'\n")
    archive = tmp_path / "full.zip"
    create_backup(state, archive)
    broken = tmp_path / "unsupported.zip"
    with zipfile.ZipFile(archive) as source, zipfile.ZipFile(broken, "w") as destination:
        for name in source.namelist():
            if name == "manifest.json":
                manifest = json.loads(source.read(name))
                manifest["schema_version"] = 0
                destination.writestr(name, json.dumps(manifest))
            else:
                with source.open(name) as input_file, destination.open(name, "w") as output:
                    shutil.copyfileobj(input_file, output)
    from asterion.runtime import backup

    with monkeypatch.context() as checks:
        checks.setattr(
            backup,
            "validate_database",
            lambda *_args, **_kwargs: pytest.fail("unsupported manifest must not start PostgreSQL"),
        )
        with pytest.raises(ValueError):
            restore_backup(broken, tmp_path / "rejected", pg)
    assert not (tmp_path / "rejected").exists()
    target = tmp_path / "restore parent 'quoted" / "recovered"
    report = restore_backup(archive, target, pg)
    assert (external / "untouched").read_bytes() == b"never use or modify this directory"
    assert len(list(external.iterdir())) == 1
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
            ExecutionFactory(),
        )
        assert research.get(job["id"])["output"] == expected
        assert DataLibrary(data_store(engine), target / "data").preview(daily["id"])["total"] == 3
        restored_url = runtime_settings(target, restored_settings).database_url
        with entry_lifecycle(restored_url, target / "data") as life:
            assert life.inspect(daily["id"])["archived"]
        sync = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            target / "data",
            provider_secrets(restored_settings["token"], target / "data"),
        )
        assert saved_values(sync, "tushare") == {"token": "offline-test-credential"}
        assert tasks.claim("must-not-resume") is None
        research.rerun(job["id"], "explicit-recovered-replay")
        claimed = tasks.claim("explicit-replay")
        assert (
            json.loads(
                execute(
                    ExecutionContext(
                        (
                            STRATEGY_RESOURCE,
                            EXECUTION,
                        ),
                        {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
                    ),
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
    return VersionAccess(reader.read, reader.coverage, reader.scan)
