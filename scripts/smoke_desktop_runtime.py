"""Test the installed runtime with a clean PATH and a fresh, isolated data directory."""

import argparse
import base64
import hashlib
import io
import json
import os
import runpy
import signal
import subprocess
import sys
import tempfile
import time
import zipfile
from decimal import Decimal
from pathlib import Path
from uuid import uuid4

import httpx

ROOT = Path(__file__).resolve().parents[1]


def fixture_identity(contract):
    return runpy.run_path(str(ROOT / "tests/import_identity_support.py"))["import_identity"](
        contract
    )


def fixture_time(contract, start, end, *, night=False):
    import re
    from datetime import date, timedelta

    from asterion.trading_time.public import TimeSpec, TimeVersion, time_id

    first, last = date.fromisoformat(start), date.fromisoformat(end)
    spec = TimeSpec.model_validate(
        {
            "schema_version": 1,
            "exchange": contract.split(".")[0],
            "product": re.sub(r"[0-9]+$", "", contract.split(".")[1]).upper(),
            "title": "隔离烟测时间",
            "timezone": "Asia/Shanghai",
            "calendar_source": "测试构造",
            "night_source": "测试构造",
            "calendar": [
                {
                    "date": (first + timedelta(days=i)).isoformat(),
                    "is_open": True,
                    "night_open": night,
                }
                for i in range(-1, (last - first).days + 1)
            ],
            "periods": [
                {
                    "start": start,
                    "end": end,
                    "source": "测试构造",
                    "day": [
                        {
                            "start": "09:00:00",
                            "end": "15:00:00",
                            "end_offset": 0,
                            "phase": "continuous",
                        }
                    ],
                    "night": [
                        {
                            "start": "21:00:00",
                            "end": "23:00:00",
                            "end_offset": 0,
                            "phase": "continuous",
                        }
                    ]
                    if night
                    else [],
                }
            ],
            "exceptions": [],
        }
    )
    return TimeVersion(id=time_id(spec), spec=spec).model_dump(mode="json")


def wait_job(client: httpx.Client, job_id: str, state: Path) -> dict:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        response = client.get(f"jobs/{job_id}")
        response.raise_for_status()
        job = response.json()
        assert "token" not in job and "payload" not in job
        if job["state"] == "SUCCEEDED":
            return job
        if job["state"] in {"FAILED", "CANCELLED"}:
            raise RuntimeError(f"Job {job_id}: {job}")
        time.sleep(0.5)
    raise RuntimeError(f"Job {job_id} timed out\n{(state / 'supervisor.log').read_text()}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    pg_root = (
        Path("/usr")
        if sys.platform == "linux"
        else Path("/opt/homebrew" if os.uname().machine == "arm64" else "/usr/local")
        / "opt/postgresql@17"
    )
    backend = runtime / "environment/bin/python"
    with tempfile.TemporaryDirectory(prefix="asterion-desktop-") as directory:
        state = Path(directory)
        # No uv, pnpm, cargo, Homebrew or project venv in PATH.
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": str(Path.home()),
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        with (state / "supervisor.log").open("w") as log:
            process = subprocess.Popen(
                [
                    str(backend),
                    "-I",
                    "-m",
                    "asterion.runtime.cli",
                    "desktop-supervise",
                    "--state",
                    str(state),
                    "--pg-root",
                    str(pg_root),
                ],
                cwd=state,
                env=env,
                stdout=log,
                stderr=log,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 60
                settings = None
                with httpx.Client(trust_env=False, timeout=2) as client:
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError((state / "supervisor.log").read_text())
                        if (state / "desktop.json").exists():
                            settings = json.loads((state / "desktop.json").read_text())
                            client.base_url = f"http://127.0.0.1:{settings['api_port']}/api/v1/"
                            client.headers["Authorization"] = f"Bearer {settings['token']}"
                            try:
                                if client.get("health").status_code == 200:
                                    break
                            except httpx.HTTPError:
                                pass
                        time.sleep(0.5)
                    else:
                        raise RuntimeError((state / "supervisor.log").read_text())
                    from asterion.runtime.build_identity import postgres_identity_roots, tree_digest

                    status = json.loads((state / "runtime-status.json").read_text())
                    assert status["build_id"] == tree_digest(
                        (
                            runtime / "environment",
                            runtime / "interpreter",
                            *postgres_identity_roots(pg_root),
                        )
                    )
                    assert client.get("snapshots").status_code == 401
                    assert client.get("account/capabilities").json()["verification"] == "local"
                    client.post(
                        "account/register",
                        json={
                            "email": "smoke@example.com",
                            "password": "synthetic-smoke-password",
                            "pin": "246810",
                            "first_name": "Smoke",
                            "last_name": "Test",
                        },
                    ).raise_for_status()
                    client.post(
                        "account/verify", json={"email": "smoke@example.com", "code": "000000"}
                    ).raise_for_status()
                    login = client.post(
                        "account/login",
                        json={"email": "smoke@example.com", "password": "synthetic-smoke-password"},
                    )
                    login.raise_for_status()
                    client.headers["X-Account-Session"] = login.json()["session"]
                    lock = client.post("account/security/lock").json()
                    assert client.get("snapshots").status_code == 423
                    assert (
                        client.post(
                            "account/security/unlock",
                            json={"pin": "000000", "expected": lock["revision"]},
                        ).status_code
                        == 401
                    )
                    unlock = client.post(
                        "account/security/unlock",
                        json={"pin": "246810", "expected": lock["revision"]},
                    )
                    unlock.raise_for_status()
                    assert not unlock.json()["locked"]
                    providers = client.get("data/providers")
                    providers.raise_for_status()
                    by_id = {provider["id"]: provider for provider in providers.json()}
                    assert set(by_id) == {"tushare"}, by_id.keys()
                    assert all(provider["api_version"] == 2 for provider in by_id.values())
                    assert not by_id["tushare"]["configured"]
                    assert client.get("data/catalog").json()["total"] == 0
                    assert len(client.get("data/types").json()) == 6
                    saved = client.post(
                        "data/providers/tushare/configuration",
                        json={"expected_revision": 0, "secrets": {"token": "synthetic-smoke-only"}},
                    )
                    saved.raise_for_status()
                    assert next(
                        p for p in client.get("data/providers").json() if p["id"] == "tushare"
                    )["configured"]
                    assert "synthetic-smoke-only" not in client.get("data/providers").text
                    client.post(
                        "data/providers/tushare/configuration",
                        json={"expected_revision": 1, "secrets": {"token": None}},
                    ).raise_for_status()
                    source = {
                        "source": "synthetic-smoke-only",
                        "source_version": "1",
                        "observed_at": "2026-01-01T00:00:00Z",
                        "available_at": "2026-01-01T00:01:00Z",
                    }
                    catalog = {
                        "schema_version": 2,
                        "inputs": [],
                        "symbols": [
                            {
                                "source": "tushare",
                                "symbol": "RB2610.SHF",
                                "contract_id": "SHFE.RB.202610.20251001",
                                "valid_from": "2025-10-01",
                                "valid_until": "2026-10-15",
                                "provenance": source,
                            }
                        ],
                        "products": [
                            {
                                "id": "SHFE.RB",
                                "exchange": "SHFE",
                                "name": "smoke",
                                "currency": "CNY",
                                "provenance": source,
                            }
                        ],
                        "contracts": [
                            {
                                "id": "SHFE.RB.202610.20251001",
                                "product_id": "SHFE.RB",
                                "delivery_month": "2026-10",
                                "listed_on": "2025-10-01",
                                "last_trade_on": "2026-10-15",
                                "last_delivery_on": None,
                                "provenance": source,
                            }
                        ],
                    }
                    publication = client.post("reference/releases", json=catalog)
                    publication.raise_for_status()
                    release_id = publication.json()["id"]
                    assert (
                        client.post("reference/releases", json=catalog).json()["id"] == release_id
                    )
                    assert (
                        client.get(f"reference/releases/{release_id}").json()["catalog"][
                            "contracts"
                        ]
                        == catalog["contracts"]
                    )
                    resolved = client.post(
                        f"reference/releases/{release_id}/resolve",
                        json={
                            "source": "tushare",
                            "symbol": "RB2610.SHF",
                            "trading_day": "2026-09-14",
                            "information_at": "2026-09-14T00:00:00Z",
                        },
                    )
                    resolved.raise_for_status()
                    assert resolved.json()["contract"]["id"] == "SHFE.RB.202610.20251001"
                    csv = (
                        "contract,event_time,available_at,open,high,low,close,volume\n"
                        "SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,3200,3220,3190,3210,100"
                    )
                    response = client.post(
                        "imports",
                        json={
                            "command_id": "installed-smoke",
                            "source": "synthetic desktop smoke test",
                            "csv": csv,
                            "options": {
                                "identity": fixture_identity("SHFE.rb2610"),
                                "source_id": "smoke",
                                "type_id": "futures.bars",
                                "timestamp_semantics": "bar_start",
                                "trading_time": fixture_time(
                                    "SHFE.rb2610", "2026-09-15", "2026-09-15", night=True
                                ),
                                "frequency": "1m",
                            },
                        },
                    )
                    response.raise_for_status()
                    job_id = response.json()["id"]
                    job = wait_job(client, job_id, state)
                    snapshot = job["result"]["snapshot_id"]
                    bars = client.get(f"snapshots/{snapshot}/bars")
                    bars.raise_for_status()
                    assert bars.json()[0]["close"] == "3210.00000000"
                    listing = client.get("data/catalog?layer=STANDARD").json()
                    assert listing["total"] == 1
                    version = listing["items"][0]
                    preview = client.get(f"data/versions/{version['id']}").json()
                    assert preview["snapshot"]["id"] == snapshot
                    raw_id = version["manifest"]["inputs"][0]
                    original = client.get(f"data/versions/{raw_id}").json()
                    assert original["rows"][0]["contract"] == "SHFE.rb2610"
                    assert original["version"]["manifest"]["layer"] == "RAW"
                    assert "trading_day" not in original["rows"][0]
                    print(
                        "PASS: installed import derives night trading day while preserving original CSV",
                        flush=True,
                    )
                    imported = {
                        "command_id": "mapped-daily-smoke",
                        "source": "离线文件示例",
                        "csv": "合约,日期,开,高,低,收,量\nSHFE.rb2610,2024-01-02,3200,3220,3190,3210,100\n",
                        "options": {
                            "identity": fixture_identity("SHFE.rb2610"),
                            "type_id": "futures.daily",
                            "frequency": "1d",
                            "source_id": "smoke_file",
                            "column_mapping": dict(
                                zip(
                                    [
                                        "contract",
                                        "trading_day",
                                        "open",
                                        "high",
                                        "low",
                                        "close",
                                        "vol",
                                    ],
                                    ["合约", "日期", "开", "高", "低", "收", "量"],
                                    strict=True,
                                )
                            ),
                        },
                    }
                    result = client.post("imports/preview", json=imported)
                    result.raise_for_status()
                    assert result.json()["valid"]
                    result = client.post("imports", json=imported)
                    result.raise_for_status()
                    wait_job(client, result.json()["id"], state)
                    listing = client.get(
                        "data/catalog?type_id=futures.daily&source=local_file&layer=STANDARD"
                    )
                    listing.raise_for_status()
                    assert (
                        listing.json()["items"][0]["manifest"]["origin"]["source_id"]
                        == "smoke_file"
                    )
                    # Three actual-code fixture rows exercise the installed research worker.
                    imported["command_id"] = "research-input-smoke"
                    imported["options"]["identity"] = fixture_identity("SHFE.rb2405")
                    imported["options"]["column_mapping"]["settle"] = "结算"
                    imported["csv"] = (
                        "合约,日期,开,高,低,收,量,结算\n"
                        "SHFE.rb2405,2024-01-02,10,10,10,10,100,10\n"
                        "SHFE.rb2405,2024-01-03,10,12,10,12,100,12\n"
                        "SHFE.rb2405,2024-01-04,20,21,20,21,100,21\n"
                    )
                    submitted = client.post("imports", json=imported)
                    submitted.raise_for_status()
                    wait_job(client, submitted.json()["id"], state)
                    daily = client.get("data/catalog?type_id=futures.daily&layer=STANDARD").json()[
                        "items"
                    ][0]
                    rejected_coverage = client.post(
                        f"data/versions/{daily['id']}/coverage",
                        json={"start": "2024-01-02", "end": "2024-01-04"},
                    )
                    assert rejected_coverage.status_code == 422
                    assert "显式关联" in rejected_coverage.json()["detail"]
                    assert (
                        client.post(
                            f"data/versions/{daily['id']}/coverage",
                            json={
                                "start": "2024-01-02",
                                "end": "2024-01-04",
                                "reference_policy": "explicit_external",
                            },
                        ).status_code
                        == 422
                    )
                    from asterion.distribution import strategy_catalog

                    discovered = client.get("research/strategies").json()
                    assert discovered == [
                        item.model_dump(mode="json") for item in strategy_catalog().list()
                    ]
                    from sqlalchemy import create_engine

                    from asterion.distribution_storage import data_storage
                    from asterion.runtime.desktop import runtime_settings

                    fixture_engine = create_engine(
                        runtime_settings(state, settings).database_url, hide_parameters=True
                    )
                    try:
                        fixed_references = runpy.run_path(str(ROOT / "tests/reference_support.py"))[
                            "reference_inputs"
                        ](data_storage(fixture_engine), state / "data")
                    finally:
                        fixture_engine.dispose()

                    def identity_report(version_id, start, end):
                        return (
                            client.post(
                                f"data/versions/{version_id}/coverage",
                                json={"start": start, "end": end, **fixed_references},
                            )
                            .raise_for_status()
                            .json()["id"]
                        )

                    research_input = {
                        "command_id": "research-smoke",
                        "version_id": daily["id"],
                        "coverage_report_id": identity_report(
                            daily["id"], "2024-01-02", "2024-01-04"
                        ),
                        "start": "2024-01-02",
                        "end": "2024-01-04",
                        "strategy": next(
                            s["identity"]
                            for s in client.get("research/strategies").json()
                            if s["identity"]["id"] == "builtin.sma-long"
                        ),
                        "parameters": {"fast": 1, "slow": 2},
                        "capital": "1000",
                        "rules": client.post(
                            "contract-rules",
                            json={
                                "contract": fixture_identity("SHFE.rb2405")["catalog"]["contracts"][
                                    0
                                ],
                                "trading_time": client.post(
                                    "trading-time",
                                    json=fixture_time(
                                        "SHFE.rb2405", "2024-01-01", "2024-12-31", night=True
                                    )["spec"],
                                )
                                .raise_for_status()
                                .json(),
                                "title": "离线烟测规则",
                                "basis": None,
                                "source": "测试构造",
                                "multiplier": "10",
                                "tick_size": "1",
                                "periods": [
                                    {
                                        "start": "2024-01-01",
                                        "end": "2024-12-31",
                                        "margin_rate": "0.1",
                                        "fee_mode": "per_lot",
                                        "open_fee": "2",
                                        "close_fee": "2",
                                        "settlement_basis": None,
                                    }
                                ],
                            },
                        )
                        .raise_for_status()
                        .json(),
                        "slippage_ticks": 1,
                        "assumption": "historical-close-unverified-calendar",
                        "coverage_policy": "allow_incomplete",
                        "coverage_note": "离线烟测构造数据，无交易日历",
                    }
                    role_spec = {
                        "schema_version": 1,
                        "origin": "provider_report",
                        "source": "offline-smoke",
                        "source_version": "offline-role-report",
                        "source_checksum": "a" * 64,
                        "observed_at": "2026-09-20T00:00:00Z",
                        "product_id": "SHFE.RB",
                        "catalog": fixture_identity("SHFE.rb2405")["catalog"],
                        "trading_time": research_input["rules"]["spec"]["trading_time"],
                        "reports": [
                            {
                                "trading_day": "2024-01-02",
                                "role": "main",
                                "contract_id": research_input["rules"]["spec"]["contract"]["id"],
                                "available_at": None,
                                "evidence": "Explicit offline smoke report",
                            }
                        ],
                    }
                    rejected_role = client.post("contract-roles", json=role_spec)
                    assert rejected_role.status_code == 422
                    wrong_role_source = client.post(
                        "contract-roles/source/preview",
                        json={
                            "mapping_version_id": daily["id"],
                            "contracts_version_id": daily["id"],
                            "trading_time": role_spec["trading_time"],
                        },
                    )
                    assert wrong_role_source.status_code == 422
                    from asterion.contract_roles.computed import algorithm_artifact
                    from asterion.contract_roles.computed_public import AlgorithmArtifact

                    packaged_algorithm = (
                        client.get("contract-roles/ranking/algorithm").raise_for_status().json()
                    )
                    assert (
                        AlgorithmArtifact.model_validate(packaged_algorithm) == algorithm_artifact()
                    )
                    assert client.get("contract-roles/computed").raise_for_status().json() == []
                    assert (
                        client.get(f"contract-roles/computed/{'a' * 64}/sync-plan").status_code
                        == 422
                    )
                    assert (
                        client.get(f"contract-roles/computed/{'a' * 64}/sync-workflows")
                        .raise_for_status()
                        .json()
                        == []
                    )
                    assert (
                        client.post(
                            "contract-roles/computed/sync-batches",
                            json={
                                "command_id": "missing-batch",
                                "previous_version_id": "a" * 64,
                                "trading_day": "2026-01-01",
                                "connection_id": None,
                                "explanation": "Explicit missing batch predecessor",
                            },
                        ).status_code
                        == 422
                    )
                    assert (
                        client.post(
                            "contract-roles/computed/sync-workflows",
                            json={
                                "command_id": "missing-workflow",
                                "previous_version_id": "a" * 64,
                                "trading_day": "2026-01-01",
                                "sync_job_ids": ["missing-one", "missing-two"],
                                "explanation": "Explicit missing dependency smoke check",
                            },
                        ).status_code
                        == 422
                    )
                    assert (
                        client.post(
                            "contract-roles/computed/tasks",
                            json={
                                "command_id": "missing-role-task",
                                "continuation": {
                                    "previous_version_id": "a" * 64,
                                    "daily_inputs": [
                                        {"trading_day": "2026-01-01", "version_id": "missing"}
                                    ],
                                    "explanation": "Explicit missing task source",
                                },
                            },
                        ).status_code
                        == 422
                    )
                    assert (
                        client.post(
                            "contract-roles/computed/continue-preview",
                            json={
                                "previous_version_id": "a" * 64,
                                "daily_inputs": [
                                    {"trading_day": "2026-01-01", "version_id": "missing"}
                                ],
                                "explanation": "Explicit missing-version smoke check",
                            },
                        ).status_code
                        == 422
                    )
                    assert (
                        client.post(
                            "contract-roles/computed/sequence/verify",
                            json={"version_ids": ["a" * 64, "b" * 64], "minimum_switches": 1},
                        ).status_code
                        == 422
                    )
                    print(
                        "PASS: installed role continuation and sequence reject missing published versions"
                    )
                    print(
                        "PASS: installed role plugin verifies packaged ranking artifact and rejects unverified sources"
                    )
                    assert (
                        client.post(
                            "research/runs",
                            json=research_input
                            | {"coverage_policy": "require_complete", "coverage_report_id": None},
                        ).status_code
                        == 422
                    )
                    assert (
                        client.post(
                            "research/runs", json=research_input | {"coverage_note": ""}
                        ).status_code
                        == 422
                    )
                    research = client.post("research/runs", json=research_input)
                    research.raise_for_status()
                    run = wait_job(client, research.json()["id"], state)
                    assert run["result"]["final_equity"] == "998"
                    detail = client.get(f"research/runs/{run['id']}").json()
                    assert detail["output"]["fills"][0]["day"] == "2024-01-04"
                    replay = client.post(
                        f"research/runs/{run['id']}/rerun",
                        json={"command_id": "research-rerun-smoke"},
                    )
                    replay.raise_for_status()
                    replayed = wait_job(client, replay.json()["id"], state)
                    assert replayed["result"]["checksum"] == run["result"]["checksum"]
                    momentum_identity = next(
                        item["identity"]
                        for item in discovered
                        if item["identity"]["id"] == "builtin.momentum-long"
                    )
                    momentum_input = research_input | {
                        "command_id": "momentum-smoke",
                        "strategy": momentum_identity,
                        "parameters": {"lookback": 1},
                    }
                    momentum = client.post("research/runs", json=momentum_input)
                    momentum.raise_for_status()
                    momentum_run = wait_job(client, momentum.json()["id"], state)
                    momentum_detail = client.get(f"research/runs/{momentum_run['id']}").json()
                    assert momentum_detail["request"]["strategy"] == momentum_identity
                    assert momentum_detail["request"]["parameters"] == {"lookback": 1}
                    assert momentum_detail["output"]["fills"][0]["day"] == "2024-01-04"
                    assert momentum_run["result"]["final_equity"] == "998"
                    momentum_package = (
                        client.get(f"research/runs/{momentum_run['id']}/export?include_data=true")
                        .raise_for_status()
                        .json()
                    )
                    assert (
                        client.post("research/packages", json=momentum_package)
                        .raise_for_status()
                        .json()["can_replay"]
                    )
                    strategy_source = ROOT / "examples/plugins/close-momentum"
                    strategy_archive = io.BytesIO()
                    with zipfile.ZipFile(strategy_archive, "w") as archive:
                        for source in sorted(strategy_source.iterdir()):
                            archive.write(source, source.name)
                    external_record = (
                        client.post(
                            "extensions/install",
                            json={
                                "archive": base64.b64encode(strategy_archive.getvalue()).decode(),
                                "digest": hashlib.sha256(strategy_archive.getvalue()).hexdigest(),
                                "trust_local_code": True,
                            },
                        )
                        .raise_for_status()
                        .json()
                    )
                    external_digest = external_record["digest"]
                    client.post(
                        "extensions/example.close_momentum/state",
                        json={"digest": external_digest, "enabled": True, "trust_local_code": True},
                    ).raise_for_status()
                    external_identity = next(
                        item["identity"]
                        for item in client.get("research/strategies").raise_for_status().json()
                        if item["identity"]["id"] == "example.close_momentum"
                    )
                    assert external_identity["digest"] == external_digest
                    external_input = momentum_input | {
                        "command_id": "external-strategy-smoke",
                        "strategy": external_identity,
                        "parameters": {
                            "lookback": 1,
                            "threshold": "0.0000",
                            "enabled": True,
                            "comparison": "strict",
                        },
                    }
                    external_job = (
                        client.post("research/runs", json=external_input).raise_for_status().json()
                    )
                    external_run = wait_job(client, external_job["id"], state)
                    assert external_run["result"]["final_equity"] == "998"
                    external_package = (
                        client.get(f"research/runs/{external_job['id']}/export?include_data=true")
                        .raise_for_status()
                        .json()
                    )
                    external_received = (
                        client.post("research/packages", json=external_package)
                        .raise_for_status()
                        .json()
                    )
                    assert external_received["can_replay"]
                    external_replayed = (
                        client.post(
                            f"research/packages/{external_received['id']}/replay",
                            json={"command_id": "external-package-smoke"},
                        )
                        .raise_for_status()
                        .json()
                    )
                    external_reproduction = wait_job(client, external_replayed["id"], state)
                    assert (
                        external_reproduction["result"]["checksum"]
                        == external_run["result"]["checksum"]
                    )
                    assert external_reproduction["result"]["reproduction_matches"] is True
                    experiment_input = {
                        "name": "installed parameter experiment",
                        "base": external_input | {"command_id": "experiment-smoke"},
                        "grid": {"enabled": [True, False]},
                    }
                    experiment = (
                        client.post("research/experiments", json=experiment_input)
                        .raise_for_status()
                        .json()
                    )
                    assert len(experiment["runs"]) == 2
                    assert (
                        client.post("research/experiments", json=experiment_input)
                        .raise_for_status()
                        .json()["runs"]
                        == experiment["runs"]
                    )
                    for experiment_run in experiment["runs"]:
                        wait_job(client, experiment_run, state)
                    experiment_result = (
                        client.get(f"research/experiments/{experiment['id']}")
                        .raise_for_status()
                        .json()
                    )
                    assert [
                        item["parameters"]["enabled"] for item in experiment_result["items"]
                    ] == [True, False]
                    assert {item["result"]["fees"] for item in experiment_result["items"]} == {
                        "0",
                        "2",
                    }
                    cancelled_experiment = (
                        client.post(f"research/experiments/{experiment['id']}/cancel", json={})
                        .raise_for_status()
                        .json()
                    )
                    assert all(
                        item["state"] == "SUCCEEDED" for item in cancelled_experiment["items"]
                    )
                    print(
                        "PASS: installed parameter experiment submits once, executes combinations and compares immutable results",
                        flush=True,
                    )
                    diagnostics = (
                        client.get("extensions/example.close_momentum/diagnostics")
                        .raise_for_status()
                        .json()
                    )
                    assert diagnostics["digest"] == external_digest
                    assert any(
                        row["phase"] == "strategy.close"
                        and row["code"] == "success"
                        and row["calls"] == 4
                        for row in diagnostics["items"]
                    )
                    assert all(
                        set(row)
                        == {"id", "digest", "started", "duration_ms", "phase", "code", "calls"}
                        for row in diagnostics["items"]
                    )
                    print(
                        "PASS: installed API reads bounded strategy session diagnostics written by worker processes",
                        flush=True,
                    )
                    client.post(
                        "extensions/example.close_momentum/state",
                        json={
                            "digest": external_digest,
                            "enabled": False,
                            "trust_local_code": False,
                        },
                    ).raise_for_status()
                    assert (
                        client.post(
                            "research/runs",
                            json=external_input | {"command_id": "disabled-external"},
                        ).status_code
                        == 422
                    )
                    print(
                        "PASS: installed multifile strategy executes in installed child, publishes and verifies replay; disable prevents execution",
                        flush=True,
                    )
                    validation_csv = (
                        "contract,trading_day,open,high,low,close,vol,settle\n"
                        + "\n".join(
                            f"SHFE.rb2405,2024-01-{i:02},{i + 10},{i + 11},{i + 10},{i + 11},100,{i + 10}.5"
                            for i in range(1, 11)
                        )
                    )
                    validation_import = (
                        client.post(
                            "imports",
                            json={
                                "command_id": "validation-data",
                                "source": "isolated validation fixture",
                                "csv": validation_csv,
                                "options": {
                                    "identity": fixture_identity("SHFE.rb2405"),
                                    "type_id": "futures.daily",
                                    "frequency": "1d",
                                    "source_id": "validation_fixture",
                                },
                            },
                        )
                        .raise_for_status()
                        .json()
                    )
                    wait_job(client, validation_import["id"], state)
                    validation_version = next(
                        v
                        for v in client.get(
                            "data/catalog?type_id=futures.daily&layer=STANDARD"
                        ).json()["items"]
                        if v["rows"] == 10
                    )
                    validation_training = (
                        client.post(
                            "research/experiments",
                            json={
                                "name": "time split",
                                "base": research_input
                                | {
                                    "command_id": "time-split",
                                    "version_id": validation_version["id"],
                                    "coverage_report_id": identity_report(
                                        validation_version["id"], "2024-01-01", "2024-01-04"
                                    ),
                                    "start": "2024-01-01",
                                    "end": "2024-01-04",
                                },
                                "grid": {"slow": [2, 3]},
                            },
                        )
                        .raise_for_status()
                        .json()
                    )
                    for training_run in validation_training["runs"]:
                        wait_job(client, training_run, state)
                    selection = {
                        "source_run": validation_training["runs"][0],
                        "start": "2024-01-05",
                        "end": "2024-01-10",
                        "coverage_report_id": identity_report(
                            validation_version["id"], "2024-01-05", "2024-01-10"
                        ),
                        "coverage_policy": "allow_incomplete",
                        "coverage_note": "isolated fixture",
                        "reason": "freeze before later-period execution",
                    }
                    validation_record = (
                        client.post(
                            f"research/experiments/{validation_training['id']}/validation",
                            json=selection,
                        )
                        .raise_for_status()
                        .json()
                    )
                    assert validation_record["evidence"]["evaluation_start"] == "2024-01-07"
                    wait_job(client, validation_record["run_id"], state)
                    ledger = (
                        client.get(f"research/runs/{validation_record['run_id']}")
                        .raise_for_status()
                        .json()["output"]["curve"]
                    )
                    assert any(Decimal(row["close_pnl"]) != 0 for row in ledger)
                    assert ledger[0]["session_open"] == "2024-01-04T21:00:00+08:00"
                    print(
                        "PASS: installed time plugin persists calendar and research opens on previous natural date night session",
                        flush=True,
                    )
                    for row in ledger:
                        assert Decimal(row["balance"]) == Decimal(row["opening_balance"]) + Decimal(
                            row["settlement_pnl"]
                        ) - Decimal(row["fees"])
                        assert Decimal(row["close_equity"]) == Decimal(row["balance"]) + Decimal(
                            row["close_pnl"]
                        )
                    print(
                        "PASS: installed daily settlement ledger reconciles; close valuation remains separate",
                        flush=True,
                    )
                    assert (
                        client.post(
                            f"research/experiments/{validation_training['id']}/validation",
                            json=selection,
                        )
                        .raise_for_status()
                        .json()["run_id"]
                        == validation_record["run_id"]
                    )
                    assert (
                        client.post(
                            f"research/experiments/{validation_training['id']}/validation",
                            json=selection | {"source_run": validation_training["runs"][1]},
                        ).status_code
                        == 409
                    )
                    exported_validation = (
                        client.get(
                            f"research/experiments/{validation_training['id']}/validation/export"
                        )
                        .raise_for_status()
                        .json()
                    )
                    with zipfile.ZipFile(
                        io.BytesIO(base64.b64decode(exported_validation["content_base64"]))
                    ) as evidence_zip:
                        for part in ("research.json", "validation.json"):
                            evidence_package = json.loads(evidence_zip.read(part))
                            received = (
                                client.post("research/packages", json=evidence_package)
                                .raise_for_status()
                                .json()
                            )
                            assert received["can_replay"]
                            replayed = (
                                client.post(
                                    f"research/packages/{received['id']}/replay",
                                    json={"command_id": f"time-split-{part}"},
                                )
                                .raise_for_status()
                                .json()
                            )
                            assert (
                                wait_job(client, replayed["id"], state)["result"][
                                    "reproduction_matches"
                                ]
                                is True
                            )
                    print(
                        "PASS: installed later-period validation locks selection, starts flat after local warmup and replays both exported periods",
                        flush=True,
                    )
                    # A structurally valid, unknown implementation must not be executed.
                    unsupported = momentum_input | {
                        "command_id": "unsupported-strategy-smoke",
                        "strategy": momentum_identity | {"digest": "0" * 64},
                    }
                    assert client.post("research/runs", json=unsupported).status_code == 422
                    print(
                        "PASS: bundled SMA and momentum plugins execute through installed worker; exact strategy identities verified; unknown implementation rejected",
                        flush=True,
                    )
                    reference = client.get(f"research/runs/{run['id']}/export")
                    reference.raise_for_status()
                    assert reference.json()["content"]["bars"] is None
                    package = client.get(f"research/runs/{run['id']}/export?include_data=true")
                    package.raise_for_status()
                    assert len(package.json()["content"]["bars"]) == 3
                    imported = client.post("research/packages", content=package.content)
                    imported.raise_for_status()
                    assert imported.json()["can_replay"]
                    reproduced = client.post(
                        f"research/packages/{imported.json()['id']}/replay",
                        json={"command_id": "research-package-smoke"},
                    )
                    reproduced.raise_for_status()
                    reproduced_run = wait_job(client, reproduced.json()["id"], state)
                    assert reproduced_run["result"]["checksum"] == run["result"]["checksum"]
                    assert reproduced_run["result"]["reproduction_matches"] is True
                    archive = client.get(f"research/runs/{run['id']}/results-archive")
                    archive.raise_for_status()
                    with zipfile.ZipFile(
                        io.BytesIO(base64.b64decode(archive.json()["content_base64"]))
                    ) as result_zip:
                        assert {"equity.csv", "positions.csv", "fills.csv", "report.md"} <= set(
                            result_zip.namelist()
                        )
                    draft_input = {
                        "expected_revision": 0,
                        "content": {
                            "config": {
                                key: value
                                for key, value in research_input.items()
                                if key != "command_id"
                            },
                            "selected_run_id": run["id"],
                        },
                    }
                    saved_draft = client.post("research/workspace/draft", json=draft_input)
                    saved_draft.raise_for_status()
                    assert saved_draft.json()["revision"] == 1
                    assert (
                        client.post("research/workspace/draft", json=draft_input).json()["revision"]
                        == 1
                    )
                    saved_workspace = client.get("research/workspace").json()
                    assert (
                        saved_workspace["draft"]["content"]["config"]["version_id"] == daily["id"]
                    )
                    template_id = str(uuid4())
                    saved_template = client.post(
                        f"research/templates/{template_id}",
                        json=draft_input | {"name": "离线回测模板"},
                    )
                    saved_template.raise_for_status()
                    assert (
                        client.get("research/workspace").json()["templates"][0]["id"] == template_id
                    )
                    assert (
                        client.post(
                            f"research/templates/{template_id}/delete",
                            json={"expected_revision": 1},
                        ).status_code
                        == 200
                    )
                    assert client.get("research/workspace").json()["templates"] == []
                    lifecycle_url = f"data/versions/{daily['id']}"
                    protection = client.get(lifecycle_url + "/lifecycle")
                    protection.raise_for_status()
                    assert protection.json()["references"]["research_runs"] >= 1
                    archived = client.post(
                        lifecycle_url + "/archive", json={"archived": True, "expected_revision": 0}
                    )
                    archived.raise_for_status()
                    assert archived.json()["archived"] and not archived.json()["can_delete"]
                    assert client.get(lifecycle_url).status_code == 200
                    assert daily["id"] not in {
                        v["id"] for v in client.get("data/catalog?layer=STANDARD").json()["items"]
                    }
                    assert daily["id"] in {
                        v["id"]
                        for v in client.get(
                            "data/catalog?layer=STANDARD&include_archived=true"
                        ).json()["items"]
                    }
                    archived_replay = client.post(
                        f"research/runs/{run['id']}/rerun",
                        json={"command_id": "archived-replay-smoke"},
                    )
                    archived_replay.raise_for_status()
                    assert (
                        wait_job(client, archived_replay.json()["id"], state)["result"]["checksum"]
                        == run["result"]["checksum"]
                    )
                    restored = client.post(
                        lifecycle_url + "/archive", json={"archived": False, "expected_revision": 1}
                    )
                    restored.raise_for_status()
                    assert not restored.json()["archived"]
                    instance = client.post(
                        "data/connections", json={"provider": "tushare", "name": "独立验证连接"}
                    )
                    instance.raise_for_status()
                    instance_id = instance.json()["id"]
                    saved = client.post(
                        f"data/providers/{instance_id}/configuration",
                        json={
                            "expected_revision": 0,
                            "values": {},
                            "secrets": {"token": "smoke-only-not-a-real-token"},
                        },
                    )
                    saved.raise_for_status()
                    assert saved.json()["configured"]
                    assert not client.get("data/providers/tushare/configuration").json()[
                        "configured"
                    ]
                    assert (
                        client.post(
                            "data/connections",
                            json={"provider": "synthetic", "name": "not shipped"},
                        ).status_code
                        == 422
                    )
                    lifecycle = client.post(
                        f"data/connections/{instance_id}",
                        json={"expected_revision": 0, "name": "归档验证连接", "state": "archived"},
                    )
                    lifecycle.raise_for_status()
                    assert lifecycle.json()["revision"] == 1
                    blocked = client.post(
                        "data/sync",
                        json={
                            "command_id": "archived-must-not-submit",
                            "provider": "tushare",
                            "connection_id": instance_id,
                            "dataset": "contracts",
                            "exchange": "SHFE",
                        },
                    )
                    assert blocked.status_code == 422
                    listed = client.get("data/providers").json()
                    assert (
                        next(p for p in listed if p["id"] == instance_id)["lifecycle"]["state"]
                        == "archived"
                    )
                    assert (
                        next(p for p in listed if p["id"] == instance_id)["verification"]["status"]
                        == "never"
                    )
                    status = client.get("services")
                    status.raise_for_status()
                    states = {item["id"]: item["state"] for item in status.json()["services"]}
                    assert all(states[key] == "ready" for key in ("api", "database", "worker"))
                    assert states["provider:tushare"] == "unconfigured"
                    assert "provider:synthetic" not in states
                    assert states["trading"] == "not_integrated"
                    plugin_source = ROOT / "examples/plugins/calendar-source"
                    archive = io.BytesIO()
                    with zipfile.ZipFile(archive, "w") as package:
                        for filename in ("manifest.json", "plugin.py"):
                            package.write(plugin_source / filename, filename)
                    installed = client.post(
                        "extensions/install",
                        json={
                            "archive": base64.b64encode(archive.getvalue()).decode(),
                            "digest": hashlib.sha256(archive.getvalue()).hexdigest(),
                            "trust_local_code": True,
                        },
                    )
                    installed.raise_for_status()
                    digest = installed.json()["digest"]
                    enabled = client.post(
                        "extensions/org.example.calendar/state",
                        json={"digest": digest, "enabled": True, "trust_local_code": True},
                    )
                    enabled.raise_for_status()
                    view = client.post(
                        "extensions/org.example.calendar/view", json={"digest": digest}, timeout=30
                    )
                    view.raise_for_status()
                    assert view.json()["rows"][0]["runtime"] == "local Python code"
                    scoped = client.post("access/scopes", json={"scope": "sources"})
                    scoped.raise_for_status()
                    assert (
                        client.get(
                            "data/providers",
                            headers={"Authorization": "Bearer " + scoped.json()["token"]},
                        ).status_code
                        == 200
                    )
                    assert (
                        client.get(
                            "extensions",
                            headers={"Authorization": "Bearer " + scoped.json()["token"]},
                        ).status_code
                        == 401
                    )
                    client.post(
                        "extensions/org.example.calendar/state",
                        json={"digest": digest, "enabled": False, "trust_local_code": False},
                    ).raise_for_status()
                    assert (
                        client.post(
                            "extensions/org.example.calendar/view", json={"digest": digest}
                        ).status_code
                        == 409
                    )
                    # Development-only data fixture; never part of the shipped provider registry.
                    fixture_factory = runpy.run_path(str(ROOT / "tests/extension_support.py"))[
                        "package_content"
                    ]
                    fixture_content = fixture_factory()
                    fixture = client.post(
                        "extensions/install",
                        json={
                            "archive": base64.b64encode(fixture_content).decode(),
                            "digest": hashlib.sha256(fixture_content).hexdigest(),
                            "trust_local_code": True,
                        },
                    )
                    fixture.raise_for_status()
                    fixture_digest = fixture.json()["digest"]
                    client.post(
                        "extensions/test.calendar/state",
                        json={"digest": fixture_digest, "enabled": True, "trust_local_code": True},
                    ).raise_for_status()
                    sync = client.post(
                        "data/sync",
                        json={
                            "command_id": "external-installed-calendar",
                            "provider": "test_calendar",
                            "dataset": "calendar",
                            "exchange": "SHFE",
                            "start": "2024-01-02",
                            "end": "2024-01-02",
                        },
                    )
                    sync.raise_for_status()
                    complete = wait_job(client, sync.json()["id"], state)
                    version = client.get("data/versions/" + complete["result"]["version_id"])
                    version.raise_for_status()
                    assert version.json()["version"]["manifest"]["plugin_digest"] == fixture_digest
                    client.post(
                        "extensions/test.calendar/state",
                        json={
                            "digest": fixture_digest,
                            "enabled": False,
                            "trust_local_code": False,
                        },
                    ).raise_for_status()
                    print(
                        "PASS: installed worker invokes external data plugin and publishes fixed package provenance"
                    )
                    print(
                        "PASS: installed independent Python plugin + bundled public SDK + declarative view + server scope enforcement + disable"
                    )
                    print(
                        "PASS: platform PostgreSQL + installed serve + installed worker + Parquet + typed catalogue + raw lineage + mapped file import + isolated provider configuration + production-only registry + installed research and exact rerun + research workspace persistence + portable package verification and replay + CSV/report export + version reference protection and reversible archive, with clean PATH"
                    )
            finally:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=40)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()

        with tempfile.TemporaryDirectory(prefix="asterion-backup-smoke-") as backup_dir:
            archive = Path(backup_dir) / "snapshot.zip"
            restored = Path(backup_dir) / "restored"

            def backup_call(role, *arguments):
                result = subprocess.run(
                    [
                        str(backend),
                        "-I",
                        "-m",
                        "asterion.runtime.cli",
                        role,
                        "--state",
                        str(state),
                        "--pg-root",
                        str(pg_root),
                        *arguments,
                    ],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=150,
                    check=False,
                )
                if result.returncode:
                    raise RuntimeError("Installed backup/restore smoke failed: " + result.stderr)
                return json.loads(result.stdout)

            saved = backup_call("desktop-snapshot", "--output", str(archive))
            assert saved["files"] > 0
            verified = backup_call(
                "desktop-restore", "--archive", str(archive), "--target", str(restored)
            )
            assert verified["status"] == "verified" and verified["research_results"] >= 1
            assert verified["research_external_not_recomputed"] == 4
            assert verified["versions"] >= 2 and verified["accounts"] == 1
            assert not (restored / "postgres/postmaster.pid").exists()
            assert (restored / "data").is_dir()
            # Exercise host indirection in the installed executable without touching launchd.
            from asterion.runtime.environments import write

            write(state, {"active": str(restored), "previous": str(state), "pending": None})
            assert backup_call("desktop-environment")["active"] == str(restored)
            assert backup_call("desktop-info")["data_directory"] == str(restored / "data")
            assert (
                backup_call("desktop-snapshot", "--output", str(Path(backup_dir) / "active.zip"))[
                    "files"
                ]
                > 0
            )
            print(
                "PASS: installed host environment selection + session endpoint + active snapshot routing"
            )
            print(
                "PASS: installed offline backup + isolated PostgreSQL restore + all catalogue hashes + research recomputation + credential decryption; original state retained"
            )


if __name__ == "__main__":
    main()
