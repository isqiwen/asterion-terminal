from copy import deepcopy

import pytest
from fastapi.testclient import TestClient
from storage_support import data_store
from test_data_sync import MASTER, context, prepared, request  # noqa: F401

from asterion.api.app import create_app
from asterion.data.backup import load_evidence, validate_backup
from asterion.data.providers.tushare import Tushare, delivery_month
from asterion.data.reference import ResolutionRequest
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.reference_store import ReferenceStore
from asterion.platform.config import Settings
from asterion.platform.files import read_files


def publish(context, monkeypatch, transform=None):  # noqa: F811
    job, content = prepared(context, monkeypatch, request("contracts"), transform)
    return context[2].publish(job["id"], job["token"], content)


def selection(version_id):
    return SourceCatalogRequest(version_id=version_id, symbols=["RB2610.SHF"])


def test_synced_version_catalog_api_and_backup(context, monkeypatch):  # noqa: F811
    engine, _, _sync, root = context
    version = publish(context, monkeypatch)
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), engine))
    endpoint = "/api/v1/reference/source"
    body = selection(version["id"]).model_dump()
    assert client.post(endpoint + "/preview", json=body).status_code == 401
    client.headers["Authorization"] = "Bearer " + MASTER
    preview = client.post(endpoint + "/preview", json=body)
    assert preview.status_code == 200, preview.text
    store = ReferenceStore(data_store(engine))
    assert store.list() == []  # Preview is read-only.
    result = client.post(endpoint + "/publish", json=body)
    assert result.status_code == 200, result.text
    release = store.get(result.json()["id"])
    assert release.catalog.model_dump(mode="json") == preview.json()
    assert client.post(endpoint + "/publish", json=body).json()["id"] == release.id
    contract = release.catalog.contracts[0]
    assert contract.id == "SHFE.RB.202610.20230101"
    assert str(contract.last_delivery_on) == "2026-10-20"
    assert release.catalog.inputs[0].checksum == version["manifest"]["checksum"]
    provenance = contract.provenance
    assert provenance.available_at.timestamp() >= version["created_at"]
    query = ResolutionRequest.model_validate(
        {
            "source": "tushare",
            "symbol": "RB2610.SHF",
            "trading_day": "2026-10-15",
            "information_at": provenance.available_at,
        }
    )
    assert release.catalog.resolve(query).contract == contract
    with pytest.raises(ValueError, match="不可知"):
        release.catalog.resolve(query.model_copy(update={"information_at": provenance.observed_at}))
    with engine.connect() as conn:
        assert store.references(conn, version["id"]) == 1
        evidence = load_evidence(conn, read_files(root), lambda b: b)
    assert validate_backup(evidence)["reference_releases"] == 1
    from dataclasses import replace

    missing = replace(
        evidence, versions=tuple(v for v in evidence.versions if v["id"] != version["id"])
    )
    with pytest.raises(ValueError, match="输入版本缺失"):
        validate_backup(missing)
    # An artifact corrupted after preview is not published on confirmation.
    (root / version["manifest"]["path"]).write_bytes(b"damaged")
    assert client.post(endpoint + "/publish", json=body).status_code == 422
    assert len(store.list()) == 1


@pytest.mark.parametrize("value", ["605", "2610", "202613", "000005", "2026-10", 202610])
def test_source_does_not_guess_delivery_year(value):
    with pytest.raises(ValueError):
        delivery_month(value)


def test_source_month_is_explicit_or_unknown():
    assert delivery_month("203605") == "2036-05"
    assert delivery_month(None) is None


@pytest.mark.parametrize(
    "damage", ["month", "delisted", "selection", "truncated", "schema", "duplicate"]
)
def test_invalid_reference_inputs_do_not_publish(context, monkeypatch, damage):  # noqa: F811
    _, _, sync, _ = context
    version = publish(context, monkeypatch)
    result = sync.library.preview(version["id"], limit=10001)
    if damage in {"month", "delisted"}:
        result["rows"][0]["delivery_month" if damage == "month" else "delisted"] = None
    elif damage == "selection":
        result["rows"][0]["symbol"] = "OTHER"
    elif damage == "truncated":
        result["total"] += 1
    elif damage == "schema":
        result["version"]["manifest"]["type"]["schema_version"] = 999
    else:
        result["rows"].append(deepcopy(result["rows"][0]))
        result["total"] += 1
    with pytest.raises(ValueError):
        source_catalog(lambda *a, **kw: result, selection(version["id"]))


def test_generic_source_reused_code_retains_two_lifecycles(context, monkeypatch):  # noqa: F811
    _, _, sync, _ = context
    version = publish(context, monkeypatch)
    result = sync.library.preview(version["id"], limit=10001)
    result["version"]["manifest"]["source"] = "independent-feed"
    first = result["rows"][0]
    first.update(
        symbol="MA605",
        exchange="CZCE",
        product="MA",
        delivery_month="2026-05",
        listed="2025-05-01",
        delisted="2026-05-15",
        last_delivery_on="2026-05-20",
    )
    second = deepcopy(first)
    second.update(
        delivery_month="2036-05",
        listed="2035-05-01",
        delisted="2036-05-15",
        last_delivery_on="2036-05-20",
    )
    result["rows"].append(second)
    result["total"] = 2
    catalog = source_catalog(
        lambda *a, **kw: result, SourceCatalogRequest(version_id=version["id"], symbols=["MA605"])
    )
    assert [c.id for c in catalog.contracts] == [
        "CZCE.MA.202605.20250501",
        "CZCE.MA.203605.20350501",
    ]
    assert {s.source for s in catalog.symbols} == {"independent-feed"}
    assert len(catalog.products) == 1


def test_tushare_keeps_reused_codes_as_separate_observations():
    from test_data_sync import raw

    req = request("contracts", exchange="CZCE")
    first = raw(Tushare().plan(req)[0])
    first.update(
        exchange="CZCE",
        ts_code="MA605.ZCE",
        fut_code="MA",
        d_month="202605",
        list_date="20250501",
        delist_date="20260515",
        last_ddate="20260520",
    )
    second = first | {
        "d_month": "203605",
        "list_date": "20350501",
        "delist_date": "20360515",
        "last_ddate": "20360520",
    }
    assert len(Tushare().normalize(req, [first, second])) == 2


@pytest.mark.parametrize("changes", [{"fut_code": "NI"}, {"d_month": "202611"}])
def test_tushare_rejects_conflicting_identity_facts(changes):
    from test_data_sync import raw

    req = request("contracts")
    row = raw(Tushare().plan(req)[0]) | changes
    with pytest.raises(ValueError, match="不一致"):
        Tushare().normalize(req, [row])
