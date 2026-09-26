"""Installed strategy artifacts execute through the public streaming SDK."""

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from asterion_bindings.diagnostics import ProcessFailure
from asterion_bindings.execution import ExecutionFactory
from test_research import payload, services  # noqa: F401

from asterion.distribution import extension_packages, strategy_catalog
from asterion.platform.extensions.process import PackageSession
from asterion.platform.serialization import canonical
from asterion.research.engine import calculate
from asterion.research.packages import ResearchPackages


@pytest.fixture
def installed(tmp_path):
    source = tmp_path / "independent-source"
    shutil.copytree(
        Path(__file__).resolve().parents[1] / "examples/strategies/close-momentum", source
    )
    archive = tmp_path / "strategy.zip"
    subprocess.run(
        [sys.executable, "-m", "asterion_plugin_sdk", "pack", str(source), str(archive)],
        check=True,
        capture_output=True,
    )
    packages = extension_packages(tmp_path / "data")
    record = packages.install(archive.read_bytes())
    catalog = strategy_catalog(tmp_path / "data")
    assert len(catalog.list()) == 2
    packages.select(record["manifest"]["id"], record["digest"], True)
    return packages, record, catalog


def external_payload(record):
    value = payload()
    value["request"].update(
        strategy={
            "id": record["manifest"]["id"],
            "version": record["manifest"]["version"],
            "digest": record["digest"],
        },
        parameters={"lookback": 1, "threshold": "0.0000", "enabled": True, "comparison": "strict"},
    )
    return value


def test_installed_multifile_strategy_and_revocation(installed):
    packages, record, catalog = installed
    assert len(catalog.list()) == 3
    value = external_payload(record)
    output = calculate(value, catalog, ExecutionFactory())
    assert output["summary"]["final_equity"] == "946"
    assert canonical(output) == canonical(calculate(value, catalog, ExecutionFactory()))
    packages.select(record["manifest"]["id"], record["digest"], False)
    assert len(catalog.list()) == 2
    with pytest.raises(ValueError):
        calculate(value, catalog, ExecutionFactory())
    packages.remove(record["manifest"]["id"], record["digest"])
    assert packages.resolve(record["digest"])[0].id == record["manifest"]["id"]


def test_tampered_dependency_rejected(installed):
    packages, record, catalog = installed
    _, path = packages.resolve(record["digest"])
    (path / "signal_logic.py").write_text("raise RuntimeError('changed dependency')")
    with pytest.raises(ValueError, match="变化"):
        calculate(external_payload(record), catalog, ExecutionFactory())


def test_streaming_session_timeout_and_live_revocation(installed):
    packages, record, _ = installed
    _, path = packages.resolve(record["digest"])
    opening = {"parameters": external_payload(record)["request"]["parameters"]}
    expired = PackageSession(path, authorized=lambda: True, timeout=0)
    with pytest.raises(ProcessFailure, match="超时") as timeout:
        expired.call("strategy.open", opening)
    assert timeout.value.code == "timeout"
    expired.close()
    expired.close()
    with pytest.raises(ProcessFailure):
        expired.call("strategy.open", opening)

    session = PackageSession(
        path, authorized=lambda: packages.enabled(record["manifest"]["id"], record["digest"])
    )
    assert session.call("strategy.open", opening) is None
    packages.select(record["manifest"]["id"], record["digest"], False)
    with pytest.raises(ProcessFailure, match="停用") as revoked:
        session.call("strategy.close", {"trading_day": "2024-01-01", "close": "10"})
    assert revoked.value.code == "revoked"

    # Restoring permission does not revive the revoked session or its strategy state.
    packages.select(record["manifest"]["id"], record["digest"], True)
    with pytest.raises(ProcessFailure):
        session.call("strategy.close", {"trading_day": "2024-01-01", "close": "10"})
    session.close()
    session.close()
    fresh = PackageSession(
        path, authorized=lambda: packages.enabled(record["manifest"]["id"], record["digest"])
    )
    try:
        assert fresh.call("strategy.open", opening) is None
    finally:
        fresh.close()


def test_external_strategy_publication_and_package_replay(request, installed):
    from storage_support import scheduler

    service, request, _ = request.getfixturevalue("services")
    _, record, catalog = installed
    service.strategies = catalog
    identity = next(s.identity for s in catalog.list() if s.identity.id == record["manifest"]["id"])
    request = request.model_copy(
        update={
            "strategy": identity,
            "parameters": {
                "lookback": 1,
                "threshold": "0.0000",
                "enabled": True,
                "comparison": "strict",
            },
        }
    )
    run = service.submit(request)
    claimed = scheduler(service.engine).claim("external")
    output = canonical(calculate(claimed["payload"], catalog, ExecutionFactory()))
    service.publish(claimed["id"], claimed["token"], output)
    bundles = ResearchPackages(service)
    exported = bundles.export(run["id"], True)
    assert exported["content"]["request"]["strategy"]["digest"] == record["digest"]
    assert bundles.receive("tester", exported)["can_replay"]


def test_backup_checks_artifact_without_executing_code(installed, services):  # noqa: F811
    from asterion.research.backup import ResearchBackup, validate_backup
    from asterion.research.external import artifact
    from asterion.research.strategies import StrategyRef

    packages, record, catalog = installed
    service, body, _ = services
    service.strategies = catalog
    settings = external_payload(record)["request"]
    value = service.prepare(
        body.model_copy(
            update={
                "strategy": StrategyRef.model_validate(settings["strategy"]),
                "parameters": settings["parameters"],
            }
        )
    )
    output = calculate(value, catalog, ExecutionFactory())
    checksum = hashlib.sha256(canonical(output)).hexdigest()
    packages.select(record["manifest"]["id"], record["digest"], False)
    checked = []

    def validate(request):
        artifact(packages, StrategyRef.model_validate(request["strategy"]))
        checked.append(request)

    evidence = ResearchBackup(
        strategy_catalog(),
        validate,
        lambda: iter([(checksum, output)]),
        lambda: iter([value]),
        lambda: iter([(value, checksum)]),
        lambda: iter(()),
        lambda: iter(()),
        execution=ExecutionFactory(),
    )
    assert validate_backup(evidence) == {
        "research_results": 1,
        "research_external_not_recomputed": 1,
    }
    assert len(checked) == 1


def test_directory_does_not_execute_installed_code(tmp_path):
    import io
    import json
    import zipfile

    source = Path(__file__).resolve().parents[1] / "examples/strategies/close-momentum"
    manifest = json.loads((source / "manifest.json").read_text())
    content = io.BytesIO()
    with zipfile.ZipFile(content, "w") as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        archive.writestr("plugin.py", "raise RuntimeError('must not execute during discovery')")
    packages = extension_packages(tmp_path)
    record = packages.install(content.getvalue())
    packages.select(manifest["id"], record["digest"], True)
    catalog = strategy_catalog(tmp_path)
    assert len(catalog.list()) == 3
    with pytest.raises(ValueError):
        calculate(external_payload(record), catalog, ExecutionFactory())
    packages.select(manifest["id"], record["digest"], False)
    manifest["version"] = "2.0.0"
    # Packages cannot depend on other installed packages: the field is not part of the contract.
    manifest["requires"] = {"example.dependency": "1.0.0"}
    content = io.BytesIO()
    with zipfile.ZipFile(content, "w") as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        archive.writestr("plugin.py", "pass")
    with pytest.raises(ValueError):
        packages.install(content.getvalue())
    assert len(packages.list()) == 1
