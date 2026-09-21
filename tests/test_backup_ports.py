from dataclasses import replace

import pytest
from cryptography.fernet import InvalidToken

from asterion.data.backup import DataBackup, validate_backup
from asterion.platform.backup import BackupCheck, validate_checks
from asterion.platform.files import read_files
from asterion.platform.plugins import Activation, Plugin


def test_read_files_stays_inside_granted_root_and_rejects_links(tmp_path):
    root = tmp_path / "data"
    root.mkdir()
    (root / "artifact").write_bytes(b"published")
    outside = tmp_path / "private"
    outside.write_bytes(b"runtime credentials")
    files = read_files(root)
    assert files.read("artifact") == b"published"
    for name in ("../private", str(outside), "./artifact", "folder/../artifact", "a\\b", "a//b"):
        with pytest.raises(ValueError):
            files.read(name)
    (root / "link").symlink_to(outside)
    with pytest.raises(ValueError, match="links"):
        files.digest("link")
    (root / "folder").symlink_to(tmp_path, target_is_directory=True)
    with pytest.raises(ValueError, match="links"):
        files.read("folder/private")


def test_data_validation_does_not_create_or_rewrite_credential_files(tmp_path):
    evidence = DataBackup((), (), (), (), read_files(tmp_path), lambda content: content)
    assert validate_backup(evidence) == {"versions": 0, "reference_releases": 0}
    assert list(tmp_path.iterdir()) == []
    credentials = tmp_path / ".credentials"
    credentials.mkdir(mode=0o750)
    encrypted = credentials / "fixture.enc"
    encrypted.write_bytes(b"damaged ciphertext")
    before = (credentials.stat().st_mode, encrypted.stat().st_mtime_ns, encrypted.read_bytes())

    def reject(content):
        raise InvalidToken()

    with pytest.raises(InvalidToken):
        validate_backup(replace(evidence, decrypt=reject))
    assert before == (
        credentials.stat().st_mode,
        encrypted.stat().st_mtime_ns,
        encrypted.read_bytes(),
    )


@pytest.mark.parametrize("invalid", ["missing", "extra", "type"])
def test_all_backup_inputs_are_checked_before_any_validator(invalid):
    calls = []
    check = BackupCheck(int, lambda value: calls.append(value))
    plugins = (
        Plugin("fixture.one", (), lambda context: Activation(), backup=check),
        Plugin("fixture.two", (), lambda context: Activation(), backup=check),
    )
    inputs = {"fixture.one": 1, "fixture.two": 2}
    if invalid == "missing":
        inputs.pop("fixture.two")
    elif invalid == "extra":
        inputs["fixture.extra"] = 3
    else:
        inputs["fixture.two"] = "wrong"
    with pytest.raises((ValueError, TypeError)):
        validate_checks(plugins, inputs)
    assert calls == []


@pytest.mark.parametrize("metrics", [{"count": -1}, {"count": True}, {"count": "one"}, None])
def test_backup_checks_reject_invalid_metrics(metrics):
    plugin = Plugin(
        "fixture.one",
        (),
        lambda context: Activation(),
        backup=BackupCheck(int, lambda value: metrics),
    )
    with pytest.raises(ValueError, match="metrics"):
        validate_checks((plugin,), {plugin.id: 1})


def test_duplicate_backup_metrics_fail_instead_of_overwriting():
    check = BackupCheck(int, lambda value: {"count": value})
    plugins = tuple(
        Plugin(f"fixture.{name}", (), lambda context: Activation(), backup=check)
        for name in ("one", "two")
    )
    with pytest.raises(ValueError, match="Duplicate"):
        validate_checks(plugins, {"fixture.one": 1, "fixture.two": 2})
