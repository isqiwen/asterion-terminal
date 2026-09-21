import base64
import copy
import io
import zipfile

import pytest
from fastapi.testclient import TestClient
from rules_support import rule_access
from sqlalchemy import create_engine, select
from storage_support import data_store, domain_tasks, raw_engine, research_store, scheduler
from test_research import services  # noqa: F401 - shared pytest fixture

from asterion.api.app import create_app
from asterion.data.library import DataLibrary, versions
from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.platform.config import Settings
from asterion.platform.store import metadata
from asterion.platform.tasks.execution import ExecutionContext
from asterion.research.packages import ResearchPackages, csv_text, digest, parse_package
from asterion.research.service import Backtests
from asterion.research.strategies import STRATEGY_RESOURCE
from asterion.research.worker import execute


@pytest.fixture
def completed(request):
    service, body, root = request.getfixturevalue("services")
    submitted = service.submit(body)
    job = scheduler(service.engine).claim("export-test")
    service.publish(
        job["id"],
        job["token"],
        execute(
            ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
            job["payload"],
        )[0],
    )
    return service, ResearchPackages(service), submitted["id"], root


def resign(value):
    value["checksum"] = digest(value["content"])
    return value


def test_default_reference_export_and_results_csv_report(completed):
    _, packages, ident, _ = completed
    value = packages.export(ident)
    assert value["content"]["bars"] is None
    assert set(value["content"]["version"]) == {"id", "dataset_id", "source", "type_id", "checksum"}
    assert value["checksum"] == digest(value["content"])
    assert packages.receive("alice", value)["detail"] == "local_run"
    with pytest.raises(KeyError):
        packages.inspect("bob", digest(value))
    archive = packages.result_archive(ident)
    with zipfile.ZipFile(io.BytesIO(base64.b64decode(archive["content_base64"]))) as z:
        assert set(z.namelist()) == {
            "rules.json",
            "coverage.json",
            "equity.csv",
            "positions.csv",
            "settlements.csv",
            "fills.csv",
            "events.csv",
            "summary.csv",
            "report.md",
        }
        assert "946" in z.read("equity.csv").decode("utf-8-sig")
        assert "SELL" in z.read("fills.csv").decode("utf-8-sig")
        assert "期末不强制平仓" in z.read("report.md").decode()
    assert "'=SUM" in csv_text([{"value": "=SUM(A1:A2)"}], ["value"])
    assert "'-cmd" in csv_text([{"value": "-cmd"}], ["value"])
    assert "'-54" not in csv_text([{"value": "-54"}], ["value"])


def test_transfer_requires_data_and_reproduces_exact_output(completed, tmp_path):
    _, source, ident, _ = completed
    engine = create_engine(f"sqlite:///{tmp_path}/other.db")
    metadata.create_all(engine)
    target = Backtests(
        research_store(engine),
        domain_tasks(research_store(engine), "research"),
        version_access(engine, tmp_path / "other-data"),
        rule_access(engine),
        strategy_catalog(),
    )
    portable = ResearchPackages(target)
    missing = portable.receive("alice", source.export(ident))
    assert missing["status"] == "MISSING_DATA" and not missing["can_replay"]
    with pytest.raises(ValueError):
        portable.replay("alice", missing["id"], "blocked")
    value = source.export(ident, True)
    ready = portable.receive("alice", value)
    assert ready["can_replay"] and ready["detail"] == "embedded"
    submitted = portable.replay("alice", ready["id"], "explicit-replay")
    assert portable.replay("alice", ready["id"], "explicit-replay")["id"] == submitted["id"]
    job = scheduler(target.engine).claim("replay")
    result = target.publish(
        job["id"],
        job["token"],
        execute(
            ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
            job["payload"],
        )[0],
    )
    assert result["checksum"] == value["content"]["output_checksum"]
    assert result["reproduction_matches"] is True
    assert target.get(job["id"])["reproduction"]["origin_run_id"] == ident
    assert portable.export(job["id"], True)["content"]["bars"] == value["content"]["bars"]
    assert (
        DataLibrary(data_store(engine), tmp_path / "other-data").list()["total"] == 0
    )  # no implicit data catalogue publication
    engine.dispose()


def test_tamper_unsupported_and_inconsistent_outputs_never_queue(completed):
    service, packages, ident, _ = completed
    value = packages.export(ident, True)
    damaged = copy.deepcopy(value)
    damaged["content"]["request"]["parameters"]["fast"] = 2
    with pytest.raises(ValueError, match="校验和"):
        packages.receive("alice", damaged)
    missing_note = copy.deepcopy(value)
    missing_note["content"]["request"]["coverage_note"] = ""
    assert packages.receive("alice", resign(missing_note))["status"] == "INVALID"
    changed = copy.deepcopy(value)
    changed["content"]["output"]["summary"]["net_profit"] = "999"
    changed["content"]["output_checksum"] = digest(changed["content"]["output"])
    assert packages.receive("alice", resign(changed))["status"] == "INVALID"
    changed = copy.deepcopy(value)
    changed["content"]["engine"] = "arbitrary.script.v99"
    report = packages.receive("alice", resign(changed))
    assert report["status"] == "UNSUPPORTED"
    with pytest.raises(ValueError, match="不支持"):
        packages.replay("alice", report["id"], "unsupported")
    changed = copy.deepcopy(value)
    changed["content"]["bars"].reverse()
    changed["content"]["bars_checksum"] = digest(changed["content"]["bars"])
    assert packages.receive("alice", resign(changed))["status"] == "INVALID"
    assert all(j["state"] == "SUCCEEDED" for j in scheduler(service.engine).list())


def test_reference_resolves_verified_local_version_and_detects_missing_file(completed):
    service, packages, ident, root = completed
    value = packages.export(ident)
    value["content"]["origin_run_id"] = "not-local"
    report = packages.receive("alice", resign(value))
    assert report["detail"] == "local_version"
    with raw_engine(service.engine).connect() as conn:
        manifest = conn.execute(
            select(versions.c.manifest).where(versions.c.id == value["content"]["version"]["id"])
        ).scalar_one()
    (root / "data" / manifest["path"]).unlink()
    assert packages.inspect("alice", report["id"])["status"] == "MISSING_DATA"


def test_json_duplicates_nonfinite_and_invalid_shapes_are_rejected(completed):
    for raw in [b'{"x":1,"x":2}', b'{"x":NaN}', b"\xff"]:
        with pytest.raises(ValueError):
            parse_package(raw)
    _, packages, ident, _ = completed
    value = packages.export(ident, True)
    value["content"]["bars"][0]["close"] = "1e999999"
    with pytest.raises(ValueError, match="结构"):
        packages.receive("alice", resign(value))


def test_package_api_gate_and_body_limit(completed):
    service, _, ident, root = completed
    client = TestClient(
        create_app(
            Settings(token="test-package-token-at-least-24", data_root=root / "data"),
            raw_engine(service.engine),
        )
    )
    assert client.get(f"/api/v1/research/runs/{ident}/export").status_code == 401
    client.headers["Authorization"] = "Bearer test-package-token-at-least-24"
    exported = client.get(f"/api/v1/research/runs/{ident}/export?include_data=true")
    assert exported.status_code == 200
    checked = client.post("/api/v1/research/packages", content=exported.content).json()
    assert checked["can_replay"]
    replay = client.post(
        f"/api/v1/research/packages/{checked['id']}/replay", json={"command_id": "http-replay"}
    )
    assert replay.status_code == 202
    assert client.post("/api/v1/research/packages", content=b" " * 8_000_001).status_code == 413


@pytest.mark.parametrize("missing", ["coverage_policy", "coverage_note", "strategy"])
def test_missing_required_parameters_prevent_replay_without_mutating_input(completed, missing):
    service, packages, ident, _ = completed
    value = packages.export(ident, True)
    value["content"]["request"].pop(missing)
    original = copy.deepcopy(value["content"]["request"])
    report = packages.receive("alice", resign(value))
    assert report["status"] in {"INVALID", "UNSUPPORTED"}
    with pytest.raises(ValueError):
        packages.replay("alice", report["id"], "unsupported-parameters")
    assert value["content"]["request"] == original
    assert len(scheduler(service.engine).list()) == 2  # file import and original research only


def version_access(engine, root):
    reader = VersionReader(data_store(engine), root)
    return VersionAccess(reader.read, reader.coverage)
