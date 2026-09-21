"""Fixed source computation and tamper checks with explicit offline observations."""

from copy import deepcopy

import pytest
from role_source_support import port, sources
from test_contract_roles import fixture
from test_role_ranking import request as diagnostic_request

from asterion.contract_roles.computed import ComputedSources, replay_computed
from asterion.contract_roles.computed_public import (
    ComputedRequest,
    ComputedVersion,
    digest,
    resolve_computed,
)
from asterion.contract_roles.public import RoleQuery
from asterion.data.public import SourceIdentity, source_contract_catalog
from asterion.data.reference_store import catalog_digest


def evidence():
    values = sources()
    first = values["contracts-1"]["rows"][0]
    second = deepcopy(first)
    second.update(
        symbol="opaque-au-next",
        contract="SHFE.au2508",
        delivery_month="2025-08",
        delisted="2025-08-15",
    )
    values["contracts-1"]["rows"].append(second)
    values["contracts-1"]["total"] = 2
    refs = []
    for day in (10, 11):
        for index, source in enumerate((first, second)):
            catalog = source_contract_catalog(port(values).read, "contracts-1", [source["symbol"]])
            identity = SourceIdentity(
                catalog_id=catalog_digest(catalog),
                catalog=catalog,
                source="offline-feed",
                symbol=source["symbol"],
                information_at="2025-04-01T00:00:00Z",
            )
            identifier = f"daily-{day}-{index}"
            observed = f"2025-04-{day}T15:05:00+08:00"
            values[identifier] = {
                "version": {
                    "id": identifier,
                    "created_at": 1,
                    "manifest": {
                        "type": {
                            "id": "futures.daily",
                            "schema_version": 1,
                            "fields": [
                                {"name": "vol", "unit": "手"},
                                {"name": "oi", "unit": "手"},
                            ],
                        },
                        "format": "parquet",
                        "source": "offline-feed",
                        "layer": "STANDARD",
                        "state": "PUBLISHED",
                        "checksum": "a" * 64,
                        "observed_at": observed,
                        "available_at": observed,
                        "contract_identity": identity.model_dump(mode="json"),
                    },
                },
                "total": 1,
                "rows": [
                    {
                        "contract": source["contract"],
                        "symbol": source["symbol"],
                        "exchange": "SHFE",
                        "trading_day": f"2025-04-{day}",
                        "open": "100",
                        "close": "100",
                        "high": "100",
                        "low": "100",
                        "vol": "100",
                        "oi": str(100 if index == 0 else 120),
                    }
                ],
            }
            refs.append({"trading_day": f"2025-04-{day}", "version_id": identifier})
    body = ComputedRequest(
        schema_version=1,
        contracts_version_id="contracts-1",
        candidate_scope={"mode": "explicit", "symbols": [first["symbol"], second["symbol"]]},
        product_id="SHFE.AU",
        trading_time=fixture().spec.trading_time,
        policy=diagnostic_request()["policy"],
        initial_main=fixture().spec.catalog.contracts[0].id,
        daily_inputs=refs,
        explanation="Explicit offline local-observation fixture",
    )
    return values, body


def test_verified_computation_replays_and_retains_local_availability():
    values, body = evidence()
    spec = ComputedSources(port(values)).build(body)
    assert spec.result.decisions[-1].reason == "switched"
    assert spec.result.decisions[-1].main == "SHFE.AU.202508.20240617"
    assert spec.artifact.sources["contract_roles/ranking.py"]
    assert replay_computed(type(spec).model_validate_json(spec.model_dump_json())) == spec.result
    assert not spec.historical_publication_attested and not spec.execution_authorized
    ComputedSources(port(values)).verify(spec)


@pytest.mark.parametrize(
    "change",
    [
        "unit",
        "missing_oi",
        "duplicate",
        "wrong_identity",
        "checksum",
        "late_created",
        "catalog",
        "partial",
        "partition_provenance",
    ],
)
def test_source_errors_fail_without_publishing(change):
    values, body = evidence()
    source = values["daily-10-0"]
    if change == "unit":
        source["version"]["manifest"]["type"]["fields"][0]["unit"] = "吨"
    elif change == "missing_oi":
        source["rows"][0]["oi"] = None
    elif change == "duplicate":
        source["rows"].append(deepcopy(source["rows"][0]))
        source["total"] = 2
    elif change == "wrong_identity":
        source["rows"][0]["contract"] = "SHFE.rb2506"
    elif change == "checksum":
        source["version"]["manifest"]["checksum"] = "bad"
    elif change == "late_created":
        source["version"]["created_at"] = 1800000000
    elif change == "catalog":
        values["contracts-1"]["rows"][0]["delisted"] = "2025-05-15"
    elif change == "partial":
        source["total"] = 10
    elif change == "partition_provenance":
        source["version"]["manifest"]["format"] = "partition_manifest"
    with pytest.raises(ValueError):
        ComputedSources(port(values)).build(body)


def test_forged_output_source_and_artifact_refused():
    values, body = evidence()
    service = ComputedSources(port(values))
    spec = service.build(body)
    changed = spec.model_dump(mode="json")
    changed["result"]["decisions"][0]["main"] = "forged"
    with pytest.raises(ValueError, match="重放"):
        service.verify(type(spec).model_validate(changed))
    changed = spec.model_dump(mode="json")
    changed["artifact"]["sources"]["contract_roles/ranking.py"] += "\n# altered\n"
    changed["artifact"]["checksum"] = digest(
        {k: v for k, v in changed["artifact"].items() if k != "checksum"}
    )
    with pytest.raises(ValueError, match="制品"):
        service.verify(type(spec).model_validate(changed))
    values["daily-10-0"]["rows"][0]["oi"] = "101"
    with pytest.raises(ValueError, match="来源证据"):
        service.verify(spec)


def test_computed_resolution_checks_publication_time_and_auction():
    values, body = evidence()
    spec = ComputedSources(port(values)).build(body)
    version = ComputedVersion(
        id=digest(spec.model_dump(mode="json")), spec=spec, published_at="2025-04-11T20:00:00+08:00"
    )
    query = RoleQuery(
        version_id=version.id,
        role="main",
        timestamp="2025-04-11T21:00:00+08:00",
        information_at="2025-04-11T21:00:00+08:00",
        mode="as_known",
        explanation="",
    )
    assert resolve_computed(version, query).contract.id == "SHFE.AU.202508.20240617"
    later = version.model_copy(update={"published_at": version.published_at.replace(year=2026)})
    with pytest.raises(ValueError, match="发布晚于"):
        resolve_computed(later, query)
    auction = query.model_copy(update={"timestamp": query.timestamp.replace(hour=20, minute=56)})
    with pytest.raises(ValueError, match="尚未生效"):
        resolve_computed(version, auction)


def test_api_append_only_auth_sources_references_backup(tmp_path, monkeypatch):
    from fastapi.testclient import TestClient
    from sqlalchemy import create_engine

    from asterion.api.app import create_app
    from asterion.contract_roles.computed_public import COMPUTED_ROLE_ACCESS
    from asterion.contract_roles.plugin import RoleBackup, validate
    from asterion.distribution_storage import role_storage
    from asterion.platform.config import Settings

    values, body = evidence()
    source_port = port(values)
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", source_port)
    )
    engine = create_engine(f"sqlite:///{tmp_path}/roles.db")
    settings = Settings(
        token="computed-roles-test-token-24", data_root=tmp_path, require_account=False
    )
    try:
        with TestClient(create_app(settings, engine)) as client:
            path = "/api/v1/contract-roles/computed"
            assert (
                client.post(path + "/preview", json=body.model_dump(mode="json")).status_code == 401
            )
            client.headers["Authorization"] = "Bearer " + settings.token
            preview = client.post(path + "/preview", json=body.model_dump(mode="json"))
            assert preview.status_code == 200, preview.text
            spec = preview.json()
            response = client.post(path, json=spec)
            assert response.status_code == 200, response.text
            saved = response.json()
            assert client.post(path, json=spec).json() == saved
            assert client.get(path).json() == [saved]
            record = ComputedVersion.model_validate(saved)
            assert client.app.state.plugins.resolve(COMPUTED_ROLE_ACCESS).read(record.id) == record
            assert client.post(path + "/replay", json=spec).json() == spec["result"]
            assert validate(RoleBackup((), source_port, (saved,), ())) == {
                "contract_role_versions": 0,
                "computed_role_versions": 1,
            }
            storage = role_storage(engine)
            with storage.connect() as conn:
                refs = [
                    hook(conn, "daily-10-0")
                    for hook in client.app.state.plugins.hooks("data.references")
                ]
            storage.close()
            assert {"contract_roles": 1} in refs
            values["daily-10-0"]["rows"][0]["oi"] = "101"
            assert client.post(path, json=spec).status_code == 422
            assert client.get(path + "/" + saved["id"]).json() == saved
            with pytest.raises(ValueError):
                validate(RoleBackup((), source_port, (saved,), ()))
    finally:
        engine.dispose()


def test_real_version_reader_partition_backup_and_corruption(tmp_path):
    import hashlib

    import pyarrow as pa
    import pyarrow.parquet as pq
    from fastapi.testclient import TestClient
    from sqlalchemy import create_engine

    from asterion.api.app import create_app
    from asterion.contract_roles.plugin import RoleBackup, validate
    from asterion.data.library import collections, versions
    from asterion.data.public import snapshot_backup_access
    from asterion.platform.config import Settings
    from asterion.platform.files import read_files
    from asterion.platform.serialization import canonical

    values, body = evidence()
    root = tmp_path / "data"
    (root / "artifacts").mkdir(parents=True)
    engine = create_engine(f"sqlite:///{tmp_path}/roles.db")
    settings = Settings(token="computed-file-test-token-24", data_root=root, require_account=False)
    app = create_app(settings, engine)

    def parquet(rows):
        buffer = pa.BufferOutputStream()
        pq.write_table(pa.Table.from_pylist(rows), buffer)
        return buffer.getvalue().to_pybytes()

    def seed(identifier, partition=False):
        source = values[identifier]
        manifest = source["version"]["manifest"]
        rows = source["rows"]
        manifest["snapshot_id"] = None
        if partition:
            data = parquet(
                [
                    r | {"_observed_at": manifest["observed_at"], "_raw_version_id": "offline-raw"}
                    for r in rows
                ]
            )
            checksum = hashlib.sha256(data).hexdigest()
            (root / "artifacts" / f"{checksum}.parquet").write_bytes(data)
            manifest["format"] = "partition_manifest"
            manifest["partitions"] = [{"key": "2025-04", "rows": len(rows), "checksum": checksum}]
            content = canonical({"schema_version": 1, "partitions": manifest["partitions"]})
        else:
            manifest["format"] = "parquet"
            content = parquet(rows)
        manifest["checksum"] = hashlib.sha256(content).hexdigest()
        manifest["path"] = f"artifacts/{identifier}.data"
        (root / manifest["path"]).write_bytes(content)
        with engine.begin() as conn:
            conn.execute(
                collections.insert().values(
                    id=identifier,
                    type_id=manifest["type"]["id"],
                    domain="market",
                    source="offline-feed",
                    layer="STANDARD",
                    identity={},
                )
            )
            conn.execute(
                versions.insert().values(
                    id=identifier,
                    dataset_id=identifier,
                    job_id="offline-fixture",
                    created_at=1,
                    rows=len(rows),
                    manifest=manifest,
                )
            )

    try:
        with TestClient(app) as client:
            seed("contracts-1")
            for ref in body.daily_inputs:
                manifest = values[ref.version_id]["version"]["manifest"]
                identity = SourceIdentity.model_validate(manifest["contract_identity"])
                catalog = source_contract_catalog(
                    port(values).read, "contracts-1", [identity.symbol]
                )
                manifest["contract_identity"] = SourceIdentity(
                    catalog_id=catalog_digest(catalog),
                    catalog=catalog,
                    source=identity.source,
                    symbol=identity.symbol,
                    information_at=identity.information_at,
                ).model_dump(mode="json")
                seed(ref.version_id, partition=True)
            client.headers["Authorization"] = "Bearer " + settings.token
            path = "/api/v1/contract-roles/computed"
            preview = client.post(path + "/preview", json=body.model_dump(mode="json"))
            assert preview.status_code == 200, preview.text
            spec = preview.json()
            saved = client.post(path, json=spec)
            assert saved.status_code == 200, saved.text
            with engine.connect() as conn:
                backup = snapshot_backup_access(conn, read_files(root))
            record = saved.json()
            assert validate(RoleBackup((), backup, (record,), ()))["computed_role_versions"] == 1
            from asterion.contract_roles.plugin import plugin
            from asterion.distribution import backup_inputs

            with engine.connect() as conn:
                assert (
                    validate(backup_inputs(conn, tmp_path, settings.token, (plugin,))[plugin.id])[
                        "computed_role_versions"
                    ]
                    == 1
                )
            bad_part = values["daily-11-1"]["version"]["manifest"]["partitions"][0]
            (root / "artifacts" / f"{bad_part['checksum']}.parquet").write_bytes(b"corrupt")
            assert client.post(path, json=spec).status_code == 422
            with pytest.raises(ValueError, match="校验和"):
                validate(RoleBackup((), backup, (record,), ()))
            assert client.get(path + "/" + record["id"]).json() == record
    finally:
        engine.dispose()
