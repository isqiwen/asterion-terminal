import base64
import hashlib
import json
import sys

import pytest
from credential_helpers import provider_secrets
from extension_support import package_content
from fastapi.testclient import TestClient
from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.data.providers import builtin_registry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.sync import collect
from asterion.distribution import extension_packages
from asterion.platform.config import Settings
from asterion.platform.extensions.packages import checked_archive
from asterion.platform.extensions.process import invoke
from asterion.platform.tasks.service import Tasks


def test_install_enable_invoke_disable_and_retained_immutable_content(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    assert not record["enabled"]
    assert [p.manifest.id for p in builtin_registry(tmp_path).all()] == ["tushare"]
    with pytest.raises(ValueError, match="刷新"):
        packages.select("test.calendar", "0" * 64, True)
    packages.select("test.calendar", record["digest"], True)
    provider = builtin_registry(tmp_path).get("test_calendar")
    assert provider.probe({}) == "插件连接检查通过"
    request = SyncRequest(
        command_id="test",
        provider="test_calendar",
        dataset="calendar",
        exchange="SHFE",
        start="2024-01-02",
        end="2024-01-02",
    )
    assert provider.plan(request)[0].limit == 100
    packages.select("test.calendar", record["digest"], False)
    with pytest.raises(ProviderError):
        builtin_registry(tmp_path).get("test_calendar")
    packages.remove("test.calendar", record["digest"])
    assert packages.list() == []
    assert packages.resolve(record["digest"])[0].version == "1.0.0"
    with pytest.raises(ValueError, match="版本"):
        packages.install(package_content(script='print("different")'))


@pytest.mark.parametrize(
    "extra", [{"../escape.py": "bad"}, {"/absolute.py": "bad"}, {"native.so": "bad"}]
)
def test_package_paths_and_native_content_are_rejected(extra):
    with pytest.raises(ValueError):
        checked_archive(package_content(extra=extra))


def test_tampered_package_never_executes(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    _, target = packages.resolve(record["digest"])
    (target / "plugin.py").write_text('raise RuntimeError("tampered")')
    with pytest.raises(ValueError, match="变化"):
        packages.select("test.calendar", record["digest"], True)
    assert not packages.list()[0]["enabled"]


def test_protocol_failure_timeout_and_environment_do_not_disclose_runtime_secrets(monkeypatch):
    monkeypatch.setenv("ASTERION_TOKEN", "test-private-master")
    monkeypatch.setenv("ASTERION_DATABASE_URL", "private-database")
    script = 'import json,sys,os; r=json.load(sys.stdin); print(json.dumps({"protocol":1,"id":r["id"],"result":list(os.environ)}))'
    result = invoke([sys.executable, "-c", script], "check", {})
    assert "ASTERION_TOKEN" not in result and "ASTERION_DATABASE_URL" not in result
    with pytest.raises(ValueError, match="超时"):
        invoke([sys.executable, "-c", "import time; time.sleep(10)"], "check", {}, timeout=0.1)
    with pytest.raises(ValueError, match="JSON"):
        invoke([sys.executable, "-c", 'print("not json")'], "check", {})
    with pytest.raises(ValueError, match="契约"):
        invoke([sys.executable, "-c", 'print("{}")'], "check", {})


def test_external_provider_api_to_fixed_job_and_validated_publication(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/state.db")
    settings = Settings(
        token="extension-integration-token-long-enough", data_root=tmp_path / "data"
    )
    app = create_app(settings, engine)
    tasks = Tasks(engine)
    try:
        with TestClient(app, headers={"Authorization": f"Bearer {settings.token}"}) as client:
            content = package_content()
            response = client.post(
                "/api/v1/extensions/install",
                json={
                    "archive": base64.b64encode(content).decode(),
                    "digest": hashlib.sha256(content).hexdigest(),
                    "trust_local_code": True,
                },
            )
            assert response.status_code == 200, response.text
            record = response.json()
            assert (
                client.post(
                    "/api/v1/extensions/test.calendar/state",
                    json={"digest": record["digest"], "enabled": True, "trust_local_code": False},
                ).status_code
                == 422
            )
            assert (
                client.post(
                    "/api/v1/extensions/test.calendar/state",
                    json={"digest": record["digest"], "enabled": True, "trust_local_code": True},
                ).status_code
                == 200
            )
            providers = client.get("/api/v1/data/providers")
            assert providers.status_code == 200, providers.text
            assert "test_calendar" in json.dumps(providers.json())
            response = client.post(
                "/api/v1/data/sync",
                json={
                    "command_id": "external",
                    "provider": "test_calendar",
                    "dataset": "calendar",
                    "exchange": "SHFE",
                    "start": "2024-01-02",
                    "end": "2024-01-02",
                },
            )
            assert response.status_code == 202, response.text
            job = tasks.claim("test-worker")
            assert job["payload"]["plugin_digest"] == record["digest"]
            content = collect(job["payload"], settings.data_root, provider_secrets(settings.token))
            published = client.post(
                f"/api/v1/jobs/{job['id']}/publish-data",
                content=content,
                headers={"X-Lease-Token": job["token"]},
            )
            assert published.status_code == 200, published.text
            assert published.json()["manifest"]["plugin_digest"] == record["digest"]
            assert tasks.get(job["id"])["state"] == "SUCCEEDED"
            assert (
                client.post(
                    "/api/v1/extensions/test.calendar/state",
                    json={"digest": record["digest"], "enabled": False, "trust_local_code": False},
                ).status_code
                == 200
            )
            assert client.get("/api/v1/data/providers").status_code == 200
            with pytest.raises(ProviderError):
                collect(job["payload"], settings.data_root, provider_secrets(settings.token))
    finally:
        engine.dispose()


def test_disable_revokes_retained_provider_and_running_process(tmp_path):
    import time
    from concurrent.futures import ThreadPoolExecutor

    packages = extension_packages(tmp_path)
    record = packages.install(package_content(script="import time; time.sleep(10)"))
    packages.select("test.calendar", record["digest"], True)
    provider = builtin_registry(tmp_path).get("test_calendar")
    with ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(provider.probe, {})
        time.sleep(0.2)
        packages.select("test.calendar", record["digest"], False)
        with pytest.raises(ProviderError):
            future.result(timeout=3)
    with pytest.raises(ProviderError):
        provider.probe({})


def test_sdk_example_pack_is_deterministic_and_declarative_view_uses_installed_contract(tmp_path):
    import subprocess
    from pathlib import Path

    source = Path(__file__).resolve().parents[1] / "examples/plugins/calendar-source"
    archives = [tmp_path / f"package-{index}.zip" for index in range(2)]
    for archive in archives:
        subprocess.run(
            [sys.executable, "-m", "asterion_plugin_sdk", "pack", str(source), str(archive)],
            check=True,
            capture_output=True,
        )
    assert archives[0].read_bytes() == archives[1].read_bytes()
    engine = create_engine(f"sqlite:///{tmp_path}/views.db")
    settings = Settings(token="view-sdk-integration-token-value", data_root=tmp_path / "data")
    try:
        with TestClient(
            create_app(settings, engine), headers={"Authorization": f"Bearer {settings.token}"}
        ) as client:
            packages = extension_packages(settings.data_root)
            record = packages.install(archives[0].read_bytes())
            packages.select("org.example.calendar", record["digest"], True)
            assert (
                client.get("/api/v1/extensions/views").json()["items"][0]["view"]["title"]
                == "插件接口信息"
            )
            result = client.post(
                "/api/v1/extensions/org.example.calendar/view", json={"digest": record["digest"]}
            )
            assert result.status_code == 200, result.text
            assert result.json()["rows"][0]["runtime"] == "local Python code"
            packages.select("org.example.calendar", record["digest"], False)
            assert (
                client.post(
                    "/api/v1/extensions/org.example.calendar/view",
                    json={"digest": record["digest"]},
                ).status_code
                == 409
            )
    finally:
        engine.dispose()


def test_dependencies_are_checked_before_enable_and_block_breaking_disable(tmp_path):
    packages = extension_packages(tmp_path)
    dependent = packages.install(
        package_content(identifier="test.dependent", requires={"test.calendar": "1.0.0"})
    )
    with pytest.raises(ValueError, match="依赖"):
        packages.select("test.dependent", dependent["digest"], True)
    base = packages.install(package_content())
    packages.select("test.calendar", base["digest"], True)
    packages.select("test.dependent", dependent["digest"], True)
    with pytest.raises(ValueError, match="依赖"):
        packages.select("test.calendar", base["digest"], False)
    assert packages.enabled("test.calendar", base["digest"])
    packages.select("test.dependent", dependent["digest"], False)
    packages.select("test.calendar", base["digest"], False)


def test_install_rejects_dependency_cycle_without_changing_receipts(tmp_path):
    packages = extension_packages(tmp_path)
    packages.install(package_content(identifier="test.one", requires={"test.two": "1.0.0"}))
    before = packages.list()
    with pytest.raises(ValueError, match="循环"):
        packages.install(package_content(identifier="test.two", requires={"test.one": "1.0.0"}))
    assert packages.list() == before


def test_inspection_has_no_installation_and_atomic_enable_rechecks_dependencies(tmp_path):
    packages = extension_packages(tmp_path)
    base = packages.install(package_content(), enabled=True)
    content = package_content(identifier="test.dependent", requires={"test.calendar": "1.0.0"})
    before = packages.list()
    inspected = packages.inspect(content)
    assert packages.list() == before
    assert not (packages.root / "objects" / inspected["digest"]).exists()
    packages.select("test.calendar", base["digest"], False)
    before = packages.list()
    with pytest.raises(ValueError, match="启用依赖"):
        packages.install(content, enabled=True)
    assert packages.list() == before
    assert not (packages.root / "objects" / inspected["digest"]).exists()
    packages.select("test.calendar", base["digest"], True)
    record = packages.install(content, enabled=True)
    assert record["enabled"]
    assert packages.install(content, enabled=True) == record


def test_install_api_requires_exact_reviewed_content_and_trust(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/state.db")
    settings = Settings(token="install-test-token-long-enough", data_root=tmp_path / "data")
    content = package_content(script='raise RuntimeError("must not execute during install")')
    encoded = base64.b64encode(content).decode()
    with TestClient(
        create_app(settings, engine), headers={"Authorization": f"Bearer {settings.token}"}
    ) as client:
        preview = client.post("/api/v1/extensions/inspect", json={"archive": encoded})
        assert preview.status_code == 200
        assert client.get("/api/v1/extensions").json()["items"] == []
        body = {"archive": encoded, "digest": preview.json()["digest"], "trust_local_code": False}
        assert client.post("/api/v1/extensions/install", json=body).status_code == 422
        body.update(trust_local_code=True, digest="0" * 64)
        assert client.post("/api/v1/extensions/install", json=body).status_code == 409
        assert client.get("/api/v1/extensions").json()["items"] == []
        body["digest"] = preview.json()["digest"]
        installed = client.post("/api/v1/extensions/install", json=body)
        assert installed.status_code == 200
        assert installed.json()["enabled"]
        assert client.post("/api/v1/extensions/install", json=body).json() == installed.json()
    engine.dispose()
