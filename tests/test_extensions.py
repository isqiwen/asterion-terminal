import base64
import io
import json
import sys
import zipfile

import pytest
from asterion_bindings.database import create_engine
from extension_support import package_content
from fastapi.testclient import TestClient

from asterion.api.app import create_app
from asterion.distribution import extension_packages
from asterion.platform.config import Settings
from asterion.platform.extensions.process import invoke
from asterion_plugin_sdk.packages import PackageManifest, checked_archive


def rewritten(content, change):
    manifest, files = checked_archive(content)
    value = manifest.model_dump()
    change(value)
    files["manifest.json"] = json.dumps(value).encode()
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as target:
        for name, data in files.items():
            target.writestr(name, data)
    return archive.getvalue()


def test_install_enable_disable_and_retained_immutable_content(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    assert not record["enabled"]
    with pytest.raises(ValueError, match="刷新"):
        packages.select("test.strategy", "0" * 64, True)
    packages.select("test.strategy", record["digest"], True)
    assert packages.enabled("test.strategy", record["digest"])
    packages.select("test.strategy", record["digest"], False)
    assert not packages.enabled("test.strategy", record["digest"])
    packages.remove("test.strategy", record["digest"])
    assert packages.list() == []
    assert packages.resolve(record["digest"])[0].version == "1.0.0"
    with pytest.raises(ValueError, match="版本"):
        packages.install(package_content(script='print("different")'))


def test_data_source_packages_and_receipts_are_rejected(tmp_path):
    def provider(value):
        value["contributions"] = {"data.provider": {"id": "test_strategy"}}

    packages = extension_packages(tmp_path)
    with pytest.raises(ValueError):
        packages.install(rewritten(package_content(), provider), enabled=True)
    assert not (tmp_path / ".extensions" / "objects").exists()
    record = packages.install(package_content())
    state = tmp_path / ".extensions" / "installed.json"
    value = json.loads(state.read_text())
    value["packages"]["test.strategy"]["manifest"]["contributions"] = {"data.provider": {}}
    state.write_text(json.dumps(value))
    with pytest.raises(ValueError, match="外部数据源插件已不再支持"):
        packages.list()
    assert packages.resolve(record["digest"])[0].id == "test.strategy"


@pytest.mark.parametrize(
    "extra", [{"../escape.py": "bad"}, {"/absolute.py": "bad"}, {"native.so": "bad"}]
)
def test_package_paths_and_native_content_are_rejected(extra):
    with pytest.raises(ValueError):
        checked_archive(package_content(extra=extra))


@pytest.mark.parametrize("layer", [None, "L0", "L1", "L2", "L4", "L5", 3])
def test_package_cannot_claim_kernel_domain_or_ui_layer(tmp_path, layer):
    def change(value):
        if layer is None:
            value.pop("layer")
        else:
            value["layer"] = layer

    root = tmp_path / "extensions"
    with pytest.raises(ValueError):
        extension_packages(root).install(rewritten(package_content(), change), enabled=True)
    assert not root.exists()


def test_package_rejects_multiple_responsibilities_and_unknown_extension_points():
    manifest, _ = checked_archive(package_content())
    value = manifest.model_dump()
    value["contributions"]["data.provider"] = {}
    with pytest.raises(ValueError):
        PackageManifest.model_validate(value)
    value["contributions"] = {"unknown.extension": {}}
    with pytest.raises(ValueError):
        PackageManifest.model_validate(value)


def test_tampered_package_never_executes(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    _, target = packages.resolve(record["digest"])
    (target / "plugin.py").write_text('raise RuntimeError("tampered")')
    with pytest.raises(ValueError, match="变化"):
        packages.select("test.strategy", record["digest"], True)
    assert not packages.list()[0]["enabled"]


def test_protocol_failure_timeout_and_environment_do_not_disclose_runtime_secrets(monkeypatch):
    monkeypatch.setenv("ASTERION_TOKEN", "test-private-master")
    monkeypatch.setenv("ASTERION_DATABASE_URL", "private-database")
    script = 'import json,sys,os; r=json.load(sys.stdin); print(json.dumps({"context":r["context"],"result":list(os.environ),"error":None}))'
    result = invoke([sys.executable, "-c", script], "check", {})
    assert "ASTERION_TOKEN" not in result and "ASTERION_DATABASE_URL" not in result
    with pytest.raises(ValueError, match="超时"):
        invoke([sys.executable, "-c", "import time; time.sleep(10)"], "check", {}, timeout=0.1)
    with pytest.raises(ValueError, match="通信契约"):
        invoke([sys.executable, "-c", 'print("not json")'], "check", {})
    with pytest.raises(ValueError, match="契约"):
        invoke([sys.executable, "-c", 'print("{}")'], "check", {})


def test_sdk_example_pack_installs_a_single_strategy(tmp_path):
    import subprocess
    from pathlib import Path

    source = Path(__file__).resolve().parents[1] / "examples/plugins/close-momentum"
    archive = tmp_path / "package.zip"
    subprocess.run(
        [sys.executable, "-m", "asterion_plugin_sdk", "pack", str(source), str(archive)],
        check=True,
        capture_output=True,
    )
    packages = extension_packages(tmp_path / "data")
    record = packages.install(archive.read_bytes(), enabled=True)
    assert record["manifest"]["layer"] == "L3"
    assert set(record["manifest"]["contributions"]) == {"research.strategy"}
    packages.select("example.close_momentum", record["digest"], False)
    assert not packages.enabled("example.close_momentum", record["digest"])


def test_inspection_has_no_installation(tmp_path):
    packages = extension_packages(tmp_path)
    content = package_content()
    inspected = packages.inspect(content)
    assert packages.list() == []
    assert not (packages.root / "objects" / inspected["digest"]).exists()
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
