import ast
from pathlib import Path


def test_business_modules_only_import_other_public_interfaces():
    root = Path(__file__).resolve().parents[1] / "src" / "asterion"
    modules = {
        "data",
        "market",
        "connections",
        "connector_ctp",
        "research",
        "trading",
        "intelligence",
        "contract_rules",
        "strategies",
        "trading_time",
    }
    for module in modules:
        for path in (root / module).rglob("*.py"):
            for node in ast.walk(ast.parse(path.read_text())):
                imports = []
                if isinstance(node, ast.ImportFrom) and node.module:
                    imports.append(node.module)
                if isinstance(node, ast.Import):
                    imports.extend(alias.name for alias in node.names)
                for name in imports:
                    parts = name.split(".")
                    if len(parts) > 1 and parts[0] == "asterion" and parts[1] in modules - {module}:
                        assert len(parts) == 3 and parts[2] == "public", (path, name)
                        assert not (module == "trading" and parts[1] == "research")


def test_platform_and_business_do_not_depend_on_application_assembly():
    root = Path(__file__).resolve().parents[1] / "src" / "asterion"
    for path in root.rglob("*.py"):
        owner = path.relative_to(root).parts[0]
        for node in ast.walk(ast.parse(path.read_text())):
            names = []
            if isinstance(node, ast.ImportFrom) and node.module:
                names = [node.module]
            elif isinstance(node, ast.Import):
                names = [alias.name for alias in node.names]
            for name in names:
                parts = name.split(".")
                if len(parts) < 2 or parts[0] != "asterion":
                    continue
                if owner == "platform":
                    assert parts[1] == "platform", (path, name)
                if owner in {
                    "data",
                    "research",
                    "identity",
                    "trading",
                    "intelligence",
                    "contract_rules",
                    "trading_time",
                }:
                    assert parts[1] not in {"api", "runtime"}, (path, name)
                if owner in {"api", "runtime"}:
                    assert parts[1] not in {
                        "data",
                        "research",
                        "identity",
                        "trading",
                        "market",
                        "connections",
                        "connector_ctp",
                        "contract_rules",
                        "trading_time",
                        "intelligence",
                    }, (path, name)
                if path == root / "runtime" / "worker.py":
                    assert parts[1] in {"platform", "runtime"}, (path, name)


def test_shared_presentation_does_not_import_panels_or_product_assembly():
    import json
    import re

    root = Path(__file__).resolve().parents[1]
    features = {
        json.loads(path.read_text())["name"]
        for base in (root / "presentation" / "panels", root / "products")
        for path in base.glob("*/package.json")
    }
    assert features
    directories = list((root / "presentation").glob("*/src"))
    assert directories and all(d.parent.name != "panels" for d in directories)
    for directory in directories:
        sources = list(directory.rglob("*.ts*"))
        assert sources, directory
        for path in sources:
            if ".test." in path.name:
                continue
            for imported in re.findall(
                r'(?:from\s*|import\s*(?:\(\s*)?)["\']([^"\']+)["\']', path.read_text()
            ):
                package = "/".join(imported.split("/")[:2])
                assert package not in features, (path, imported)
                if imported.startswith("."):
                    target = (path.parent / imported).resolve()
                    assert not target.is_relative_to(root / "presentation" / "panels"), (
                        path,
                        imported,
                    )
                    assert not target.is_relative_to(root / "products"), (path, imported)


def test_feature_request_consumers_use_credential_free_ports():
    import re

    root = Path(__file__).resolve().parents[1]
    transport = {
        "@asterion/runtime-client/client",
        "@asterion/runtime-client/useRequestClient",
        "@asterion/desktop-bridge/useConnection",
        "@asterion/desktop-bridge/desktop",
    }
    transport_sources = {
        root / "presentation" / package / "src" / module
        for package, module in (
            ("runtime-client", "client"),
            ("runtime-client", "useRequestClient"),
            ("desktop-bridge", "useConnection"),
            ("desktop-bridge", "desktop"),
        )
    }
    for domain in ("data-panel", "research-panel", "task-center"):
        sources = list((root / "presentation" / "panels" / domain / "src").rglob("*.ts*"))
        assert sources, domain
        for path in sources:
            if ".test." in path.name:
                continue
            for imported in re.findall(
                r'(?:from\s*|import\s*(?:\(\s*)?)["\']([^"\']+)["\']', path.read_text()
            ):
                assert imported not in transport, (path, imported)
                if imported.startswith("."):
                    target = (path.parent / imported).resolve().with_suffix("")
                    assert target not in transport_sources, (path, imported)


def test_feature_worker_entries_do_not_import_runtime_configuration_or_transport():
    root = Path(__file__).resolve().parents[1] / "src" / "asterion"
    for domain in ("research",):
        path = root / domain / "worker.py"
        for node in ast.walk(ast.parse(path.read_text())):
            names = []
            if isinstance(node, ast.ImportFrom) and node.module:
                names = [node.module]
            elif isinstance(node, ast.Import):
                names = [alias.name for alias in node.names]
            for name in names:
                assert name not in {
                    "httpx",
                    "asterion.platform.config",
                    "asterion_bindings.secrets",
                }, (path, name)
