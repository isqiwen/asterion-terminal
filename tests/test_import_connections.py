import copy

import pytest
from credential_helpers import provider_secrets
from fastapi.testclient import TestClient
from import_identity_support import import_identity
from sqlalchemy import create_engine
from storage_support import data_store, domain_tasks, raw_engine, scheduler
from test_provider_configuration import demo_request

from asterion.api.app import create_app
from asterion.data.configuration import ConfigurationUpdate
from asterion.data.connections import NewConnection
from asterion.data.importing import encode_import, preview
from asterion.data.public import ImportOptions
from asterion.data.snapshots import Snapshots
from asterion.data.sync import DataSync, collect
from asterion.platform.config import Settings
from asterion.platform.store import metadata

TOKEN = "isolated-import-test-token-long-enough"
CSV = "合约,日期,开,高,低,收,量\nSHFE.rb2610,2024-01-02,3200,3220,3190,3210,100\n"
MAPPING = dict(
    zip(
        ["contract", "trading_day", "open", "high", "low", "close", "vol"],
        ["合约", "日期", "开", "高", "低", "收", "量"],
        strict=True,
    )
)


def options(source_id="vendor_a"):
    return ImportOptions(
        identity=import_identity("SHFE.rb2610"),
        type_id="futures.daily",
        frequency="1d",
        source_id=source_id,
        column_mapping=MAPPING,
    )


@pytest.fixture
def services(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/data.db")
    metadata.create_all(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path / "data",
        provider_secrets(TOKEN),
    )
    yield (
        sync,
        Snapshots(data_store(engine), domain_tasks(data_store(engine), "data"), tmp_path / "data"),
    )
    engine.dispose()


def test_mapping_preview_uses_daily_type_and_keeps_rows():
    result = preview(CSV, options())
    assert result.valid and result.total == 1
    assert result.rows[0]["exchange"] == "SHFE"
    assert result.rows[0]["vol"] == "100"
    wrong = options().model_copy(update={"column_mapping": {"close": "收"}})
    assert not preview(CSV, wrong).valid


@pytest.mark.parametrize(
    "text",
    [
        CSV.replace("量", "收"),
        CSV.replace(",100", ",100,extra"),
        CSV.replace("2024-01-02", "2999-01-02"),
        CSV.replace("3210", "oops"),
        CSV + CSV.splitlines()[1] + "\n",
    ],
)
def test_invalid_files_do_not_publish(text):
    with pytest.raises(ValueError):
        encode_import({"csv": text, "options": options().model_dump(mode="json")})


def test_daily_file_versions_and_raw_provenance(services):
    sync, snapshots = services
    dataset_ids = []
    for index, source_id in enumerate(["vendor_a", "vendor_a", "vendor_b"]):
        payload = {
            "source": "真实供应商历史文件",
            "csv": CSV,
            "options": options(source_id).model_dump(mode="json"),
        }
        sync.tasks.submit(f"import-{index}", "data.import_csv", payload)
        job = scheduler(sync.engine).claim("worker")
        content, _ = encode_import(job["payload"])
        record = snapshots.publish(job["id"], job["token"], content)
        assert snapshots.publish(job["id"], job["token"], content)["id"] == record["id"]
        assert record["manifest"]["time_semantics"] == "trading_day_label"
        versions = sync.library.list(type_id="futures.daily", layer="STANDARD")["items"]
        version = next(v for v in versions if v["job_id"] == job["id"])
        dataset_ids.append(version["dataset_id"])
        assert version["manifest"]["origin"]["source_id"] == source_id
        assert sync.library.preview(version["id"])["rows"][0]["close"] == "3210"
        raw = sync.library.preview(version["manifest"]["inputs"][0])
        assert raw["rows"][0]["合约"] == "SHFE.rb2610"
    assert dataset_ids[0] == dataset_ids[1] != dataset_ids[2]


def test_two_connections_freeze_config_and_separate_versions(services):
    sync, _ = services
    a = sync.connections.create(NewConnection(provider="synthetic", name="研究 A"))
    b = sync.connections.create(NewConnection(provider="synthetic", name="研究 B"))
    for instance, seed in ((a, 7), (b, 17)):
        sync.configuration.apply(
            instance["id"], ConfigurationUpdate(expected_revision=0, values={"seed": seed})
        )
    requests = [
        demo_request("a").model_copy(update={"connection_id": a["id"]}),
        demo_request("b").model_copy(update={"connection_id": b["id"]}),
    ]
    for req in requests:
        sync.submit(req)
    # Already submitted work retains seed 7.
    sync.configuration.apply(a["id"], ConfigurationUpdate(expected_revision=1, values={"seed": 27}))
    results = []
    for _ in requests:
        job = scheduler(sync.engine).claim("worker")
        content = collect(job["payload"], sync.root, provider_secrets(TOKEN))
        record = sync.publish(job["id"], job["token"], content)
        results.append(record)
    assert results[0]["dataset_id"] != results[1]["dataset_id"]
    assert sync.library.list(source=a["id"], layer="STANDARD")["total"] == 1
    assert sync.configuration.current("synthetic")[1]["seed"] == 7
    job = sync.submit(requests[0])
    payload = copy.deepcopy(job["payload"])
    payload["request"]["connection_id"] = b["id"]
    with pytest.raises(ValueError, match="固定配置"):
        collect(payload, sync.root, provider_secrets(TOKEN))
    payload = copy.deepcopy(job["payload"])
    del payload["configuration"]
    with pytest.raises(ValueError, match="固定配置"):
        collect(payload, sync.root, provider_secrets(TOKEN))


def test_connection_and_preview_api(services):
    sync, _ = services
    client = TestClient(
        create_app(Settings(token=TOKEN, data_root=sync.root), raw_engine(sync.engine)),
        headers={"Authorization": "Bearer " + TOKEN},
    )
    response = client.post(
        "/api/v1/data/connections", json={"provider": "synthetic", "name": "独立示例"}
    )
    assert response.status_code == 201
    identifier = response.json()["id"]
    provider = next(p for p in client.get("/api/v1/data/providers").json() if p["id"] == identifier)
    assert provider["plugin_id"] == "synthetic"
    assert client.get(f"/api/v1/data/providers/{identifier}/configuration").json()["values"] == {
        "seed": 7
    }
    response = client.post(
        "/api/v1/imports/preview",
        json={
            "command_id": "preview",
            "source": "文件",
            "csv": CSV,
            "options": options().model_dump(mode="json"),
        },
    )
    assert response.status_code == 200 and response.json()["valid"]
    assert scheduler(sync.engine).list() == []


def test_coverage_references_do_not_cross_connections(services, monkeypatch):
    from configuration_support import set_token
    from sync_identity_support import submit_source
    from test_data_sync import raw, request

    from asterion.data.coverage import CoverageRequest
    from asterion.data.providers.tushare import Tushare

    sync, _ = services
    a = sync.connections.create(NewConnection(provider="tushare", name="A"))
    b = sync.connections.create(NewConnection(provider="tushare", name="B"))
    for instance in (a, b):
        set_token(sync, instance["id"], "fixture-token")
    monkeypatch.setattr(Tushare, "fetch", lambda self, part, config: [raw(part)])
    records = {}
    for instance, dataset in ((a, "daily"), (a, "calendar"), (b, "calendar")):
        req = request(dataset, command_id=instance["id"] + dataset, connection_id=instance["id"])
        submit_source(sync, req)
        job = scheduler(sync.engine).claim("worker")
        records[(instance["id"], dataset)] = sync.publish(
            job["id"], job["token"], collect(job["payload"], sync.root, provider_secrets(TOKEN))
        )
    daily = records[(a["id"], "daily")]["id"]
    report = sync.coverage.check(daily, CoverageRequest(start="2024-01-02", end="2024-01-05"))
    assert report["calendar_version_id"] == records[(a["id"], "calendar")]["id"]
    assert report["connection_id"] == a["id"]
    with pytest.raises(ValueError, match="来源及交易所"):
        sync.coverage.check(
            daily,
            CoverageRequest(
                start="2024-01-02",
                end="2024-01-05",
                calendar_version_id=records[(b["id"], "calendar")]["id"],
            ),
        )


def test_type_registry_can_load_first_in_fresh_worker():
    import subprocess
    import sys

    subprocess.run(
        [
            sys.executable,
            "-c",
            "from asterion.data.types import builtin_types; assert len(builtin_types().all()) == 6",
        ],
        check=True,
    )


pytestmark = pytest.mark.usefixtures("development_providers")
