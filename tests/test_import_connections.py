import copy

import pytest
from asterion_bindings.database import create_engine
from credential_helpers import provider_secrets
from import_identity_support import import_identity
from import_support import import_options
from storage_support import data_store, domain_tasks, scheduler

from asterion.data.sync import DataSync, fixed_configuration
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
    return import_options(
        import_identity("SHFE.rb2610"),
        type_id="futures.daily",
        frequency="1d",
        source_id=source_id,
        column_mapping=MAPPING,
    )


@pytest.fixture
def services(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/data.db")
    metadata.create_all(engine)
    (tmp_path / "data").mkdir()
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path / "data",
        provider_secrets(TOKEN, tmp_path / "data"),
    )
    yield sync
    engine.dispose()


def test_two_connections_freeze_config_and_separate_versions(services, monkeypatch):
    from test_data_sync import evidence_content, request

    from asterion.data.providers.tushare import Tushare

    sync = services
    a = sync.sources.create("tushare", "研究 A")
    b = sync.sources.create("tushare", "研究 B")
    for instance, token in ((a, "token-a-original"), (b, "token-b")):
        sync.sources.apply(instance["id"], 0, secrets={"token": token})
    requests = [
        request("calendar", command_id="a", connection_id=a["id"]),
        request("calendar", command_id="b", connection_id=b["id"]),
    ]
    for req in requests:
        sync.submit(req)
    # Already submitted work keeps the configuration it was fixed to.
    sync.sources.apply(a["id"], 1, secrets={"token": "token-a-changed"})
    results, seen = [], []
    for _ in requests:
        job = scheduler(sync.engine).claim("worker")
        seen.append(fixed_configuration(sync.credentials, job["payload"], Tushare())["token"])
        results.append(sync.publish(job["id"], job["token"], evidence_content(job)))
    assert sorted(seen) == ["token-a-original", "token-b"]
    assert results[0]["dataset_id"] != results[1]["dataset_id"]
    assert sync.library.list(source="tushare", layer="STANDARD")["total"] == 2
    job = sync.submit(requests[0])
    payload = copy.deepcopy(job["payload"])
    payload["request"]["connection_id"] = b["id"]
    with pytest.raises(ValueError, match="固定配置"):
        fixed_configuration(sync.credentials, payload, Tushare())
    payload = copy.deepcopy(job["payload"])
    del payload["configuration"]
    with pytest.raises(ValueError, match="固定配置"):
        fixed_configuration(sync.credentials, payload, Tushare())


def test_coverage_references_do_not_cross_connections(services, monkeypatch):
    from configuration_support import set_token
    from sync_identity_support import submit_source
    from test_data_sync import evidence_content, request

    from asterion.data.coverage import CoverageRequest

    sync = services
    a = sync.sources.create("tushare", "A")
    b = sync.sources.create("tushare", "B")
    for instance in (a, b):
        set_token(sync, instance["id"], "fixture-token")
    records = {}
    for instance, dataset in ((a, "daily"), (a, "calendar"), (b, "calendar")):
        req = request(dataset, command_id=instance["id"] + dataset, connection_id=instance["id"])
        submit_source(sync, req)
        job = scheduler(sync.engine).claim("worker")
        records[(instance["id"], dataset)] = sync.publish(
            job["id"],
            job["token"],
            evidence_content(job),
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
            "from asterion.data.types import builtin_types; assert len(builtin_types().all()) == 7",
        ],
        check=True,
    )
