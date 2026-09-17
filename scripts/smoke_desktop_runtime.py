"""Test the bundled executable with a clean PATH and a fresh, isolated data directory."""

import argparse
import json
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, default=ROOT / "apps/terminal/src-tauri/runtime")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    backend = runtime / "asterion-backend/asterion-backend"
    with tempfile.TemporaryDirectory(prefix="asterion-desktop-") as directory:
        state = Path(directory)
        # No uv, pnpm, cargo, Homebrew or project venv in PATH.
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": str(Path.home()),
            "PYINSTALLER_RESET_ENVIRONMENT": "1",
        }
        with (state / "supervisor.log").open("w") as log:
            process = subprocess.Popen(
                [
                    str(backend),
                    "desktop-supervise",
                    "--state",
                    str(state),
                    "--pg-root",
                    str(runtime / "postgres"),
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
                    assert [p["id"] for p in providers.json()] == ["tushare"]
                    assert not providers.json()[0]["configured"]
                    assert client.get("data/catalog").json()["total"] == 0
                    assert len(client.get("data/types").json()) == 4
                    saved = client.post(
                        "data/providers/tushare/credential", json={"token": "synthetic-smoke-only"}
                    )
                    saved.raise_for_status()
                    assert client.get("data/providers").json()[0]["configured"]
                    assert "synthetic-smoke-only" not in client.get("data/providers").text
                    client.post(
                        "data/providers/tushare/credential", json={"token": ""}
                    ).raise_for_status()
                    catalog = {"products": [], "contracts": [], "calendars": [], "rules": []}
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
                        == []
                    )
                    csv = (
                        "contract,event_time,available_at,trading_day,open,high,low,close,volume\n"
                        "SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100"
                    )
                    response = client.post(
                        "imports",
                        json={
                            "command_id": "frozen-smoke",
                            "source": "synthetic desktop smoke test",
                            "csv": csv,
                        },
                    )
                    response.raise_for_status()
                    job_id = response.json()["id"]
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        jobs = client.get("jobs").json()
                        job = next(j for j in jobs if j["id"] == job_id)
                        if job["state"] == "SUCCEEDED":
                            break
                        if job["state"] == "FAILED":
                            raise RuntimeError(job["error"])
                        time.sleep(0.5)
                    else:
                        raise RuntimeError((state / "supervisor.log").read_text())
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
                    print(
                        "PASS: bundled PostgreSQL + frozen serve + frozen worker + Parquet + typed catalogue + raw lineage, with clean PATH"
                    )
            finally:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=40)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


if __name__ == "__main__":
    main()
