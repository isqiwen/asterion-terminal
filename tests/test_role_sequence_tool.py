"""Local acceptance reads the owner's records and never repairs or republishes evidence."""

import importlib.util
from pathlib import Path

import pytest
from role_source_support import port
from sqlalchemy import create_engine, insert, select
from test_role_sequence import campaign

from asterion.contract_roles.plugin import computed_versions
from asterion.contract_roles.sequence import SequenceRequest


@pytest.fixture
def tool():
    path = Path(__file__).resolve().parents[1] / "scripts/verify_role_sequence.py"
    spec = importlib.util.spec_from_file_location("verify_role_sequence", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("failure", [None, "source", "order", "missing", "switches"])
def test_verification_retains_original_records(tool, monkeypatch, failure):
    values, _, first, second, _ = campaign()
    engine = create_engine("sqlite://")
    computed_versions.create(engine)
    with engine.begin() as connection:
        connection.execute(
            insert(computed_versions), [r.model_dump(mode="json") for r in (first, second)]
        )
    monkeypatch.setattr(tool, "snapshot_backup_access", lambda *_: port(values))
    ids = (first.id, second.id)
    if failure == "source":
        values["daily-10-0"]["rows"][0]["oi"] = "99"
    elif failure == "order":
        ids = ids[::-1]
    elif failure == "missing":
        ids = ("f" * 64, second.id)
    request = SequenceRequest(version_ids=ids, minimum_switches=2 if failure == "switches" else 1)
    with engine.connect() as connection:
        original = list(connection.execute(select(computed_versions)).mappings())
        if failure:
            with pytest.raises(ValueError):
                tool.inspect_evidence(connection, None, request)
        else:
            result = tool.inspect_evidence(connection, None, request)
            assert result["status"] == "VERIFIED"
            assert result["result"]["switches"] == 1
            assert not result["live_collection_attested"]
            assert not result["result"]["execution_authorized"]
        assert list(connection.execute(select(computed_versions)).mappings()) == original
    engine.dispose()


def test_empty_database_cannot_pass_acceptance(tool, monkeypatch):
    engine = create_engine("sqlite://")
    computed_versions.create(engine)
    monkeypatch.setattr(tool, "snapshot_backup_access", lambda *_: pytest.fail("No sources needed"))
    with engine.connect() as connection:
        result = tool.inspect_evidence(connection, None, None)
        assert result["status"] == "NOT_VERIFIED"
        assert result["available_versions"] == []
    engine.dispose()
