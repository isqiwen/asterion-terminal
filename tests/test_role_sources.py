from copy import deepcopy

import pytest
from fastapi.testclient import TestClient
from role_source_support import port, sources
from rules_support import time_version
from test_contract_roles import fixture
from test_data_sync import MASTER, context, prepared, request  # noqa: F401

from asterion.api.app import create_app
from asterion.contract_roles.public import RoleQuery, RoleVersion, resolve, role_id
from asterion.contract_roles.sources import RoleSourceRequest, RoleSources
from asterion.data.providers.public import ProviderError
from asterion.data.providers.tushare import Tushare
from asterion.platform.config import Settings


def source_request():
    return RoleSourceRequest(
        mapping_version_id="report-1",
        contracts_version_id="contracts-1",
        trading_time=fixture().spec.trading_time,
    )


def test_independent_source_maps_opaque_codes_without_provider_branches():
    reader = RoleSources(port(sources()))
    spec = reader.build(source_request())
    assert spec.reports[0].contract_id == "SHFE.AU.202506.20240617"
    assert spec.source == "offline-feed"
    reader.verify(spec)
    changed = spec.model_dump(mode="json")
    changed["reports"][0]["evidence"] = "forged"
    with pytest.raises(ValueError, match="来源证据"):
        reader.verify(type(spec).model_validate(changed))


@pytest.mark.parametrize(
    "mutation",
    [
        "source",
        "type",
        "truncated",
        "empty",
        "target",
        "date",
        "product",
        "duplicate",
        "checksum",
        "catalog",
    ],
)
def test_publication_refuses_bad_or_tampered_source(mutation):
    values = sources()
    reader = RoleSources(port(values))
    spec = reader.build(source_request())
    mapping = values["report-1"]
    if mutation == "source":
        mapping["version"]["manifest"]["source"] = "other"
    if mutation == "type":
        mapping["version"]["manifest"]["type"]["id"] = "futures.daily"
    if mutation == "truncated":
        mapping["total"] = 10001
    if mutation == "empty":
        mapping.update(total=0, rows=[])
    if mutation == "target":
        mapping["rows"][0]["target_symbol"] = "missing"
    if mutation == "date":
        mapping["rows"][0]["trading_day"] = "2026-04-14"
    if mutation == "product":
        mapping["rows"][0]["product_id"] = "SHFE.RB"
    if mutation == "duplicate":
        mapping["rows"] *= 2
        mapping["total"] = 2
    if mutation == "checksum":
        mapping["version"]["manifest"]["checksum"] = "b" * 64
    if mutation == "catalog":
        values["contracts-1"]["version"]["manifest"]["checksum"] = "b" * 64
    before = deepcopy(values)
    with pytest.raises(ValueError):
        reader.verify(spec)
    assert values == before


@pytest.mark.parametrize("symbol", ["RB2610.SHF", "RB.SHF0", "RB.DCE", "rb.SHF"])
def test_tushare_mapping_rejects_non_main_codes(symbol):
    with pytest.raises(ProviderError):
        Tushare().plan(request("mapping", symbol=symbol))


def test_mapping_plan_preserves_unknown_time_and_rejects_wrong_target():
    req = request("mapping", symbol="RB.SHF", end="2024-03-31")
    parts = Tushare().plan(req)
    assert len(parts) == 3
    assert all(
        p.api == "fut_mapping" and p.limit == 2000 and "exchange" not in p.params for p in parts
    )
    raw = {"ts_code": "RB.SHF", "mapping_ts_code": "RB2610.SHF", "trade_date": "20240102"}
    assert Tushare().normalize(req, [raw])[0]["available_at"] is None
    for changes in (
        {"mapping_ts_code": "CU2610.SHF"},
        {"ts_code": "CU.SHF"},
        {"trade_date": "20240101"},
    ):
        with pytest.raises(ProviderError):
            Tushare().normalize(req, [raw | changes])
    with pytest.raises(ProviderError):
        Tushare().normalize(req, [raw, raw])


def test_published_mapping_preview_save_and_source_corruption(context, monkeypatch):  # noqa: F811
    engine, _, sync, root = context
    job, content = prepared(context, monkeypatch, request("contracts", command_id="contracts"))
    contracts = sync.publish(job["id"], job["token"], content)

    def mapping_rows(rows):
        return [{"ts_code": "RB.SHF", "mapping_ts_code": "RB2610.SHF", "trade_date": "20240102"}]

    job, content = prepared(
        context,
        monkeypatch,
        request("mapping", symbol="RB.SHF", command_id="mapping"),
        mapping_rows,
    )
    mapping = sync.publish(job["id"], job["token"], content)
    assert mapping["manifest"]["type"]["id"] == "futures.role_mapping"
    assert mapping["manifest"]["coverage"] == "RETURNED_ROWS_ONLY"
    assert sync.library.preview(mapping["id"])["snapshot"] is None
    body = RoleSourceRequest(
        mapping_version_id=mapping["id"],
        contracts_version_id=contracts["id"],
        trading_time=time_version("SHFE.rb2610"),
    )
    with TestClient(
        create_app(Settings(token=MASTER, data_root=root, require_account=False), engine)
    ) as client:
        path = "/api/v1/contract-roles"
        assert (
            client.post(path + "/source/preview", json=body.model_dump(mode="json")).status_code
            == 401
        )
        client.headers["Authorization"] = "Bearer " + MASTER
        response = client.post(path + "/source/preview", json=body.model_dump(mode="json"))
        assert response.status_code == 200, response.text
        spec = response.json()
        assert spec["reports"][0]["available_at"] is None
        saved = client.post(path, json=spec)
        assert saved.status_code == 200, saved.text
        version = RoleVersion.model_validate(saved.json())
        query = RoleQuery(
            version_id=version.id,
            role="main",
            timestamp="2024-01-02T09:00:00+08:00",
            information_at="2024-01-02T09:00:00+08:00",
            mode="as_known",
            explanation="",
        )
        with pytest.raises(ValueError, match="可知"):
            resolve(version, query)
        changed = deepcopy(spec)
        changed["source_checksum"] = "0" * 64
        assert client.post(path, json=changed).status_code == 422
        for identifier in (mapping["id"], contracts["id"]):
            response = client.get(f"/api/v1/data/versions/{identifier}/lifecycle")
            assert response.json()["references"]["contract_roles"] == 1
        from asterion.contract_roles.plugin import RoleBackup, validate
        from asterion.data.public import snapshot_backup_access
        from asterion.platform.files import read_files

        with engine.connect() as conn:
            backup_port = snapshot_backup_access(conn, read_files(root))
        assert validate(RoleBackup((version.model_dump(mode="json"),), backup_port, (), ())) == {
            "contract_role_versions": 1,
            "computed_role_versions": 0,
        }
        changed = version.spec.model_dump(mode="json")
        changed["source_checksum"] = "0" * 64
        changed_spec = type(version.spec).model_validate(changed)
        forged = RoleVersion(id=role_id(changed_spec), spec=changed_spec).model_dump(mode="json")
        with pytest.raises(ValueError, match="来源证据"):
            validate(RoleBackup((forged,), backup_port, (), ()))
        from sqlalchemy import select

        from asterion.data.library import versions

        with engine.connect() as conn:
            manifest = conn.execute(
                select(versions.c.manifest).where(versions.c.id == mapping["id"])
            ).scalar_one()
        (root / manifest["path"]).write_bytes(b"damaged")
        assert client.post(path, json=spec).status_code == 422
        assert client.get(path).json() == [version.model_dump(mode="json")]
