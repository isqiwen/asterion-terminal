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


def test_terminal_mechanisms_do_not_import_feature_plugins_or_product_assembly():
    import re

    root = Path(__file__).resolve().parents[1] / "apps" / "terminal" / "src"
    for directory in (
        "api",
        "components",
        "extensions",
        "settings",
        "workspace",
        "startup",
        "deployment",
    ):
        for path in (root / directory).rglob("*.ts*"):
            if ".test." in path.name:
                continue
            for imported in re.findall(
                r'(?:from\s*|import\s*)["\']([^"\']+)["\']', path.read_text()
            ):
                if not imported.startswith("."):
                    continue
                target = (path.parent / imported).resolve()
                assert not target.is_relative_to(root / "plugins"), (path, imported)
                assert target != root / "distribution", (path, imported)


def test_feature_request_consumers_use_credential_free_ports():
    import re

    root = Path(__file__).resolve().parents[1] / "apps" / "terminal" / "src"
    transport = {root / "api" / name for name in ("client", "useConnection", "useRequestClient")}
    for domain in ("data", "research", "tasks"):
        for path in (root / "plugins" / domain).rglob("*.ts*"):
            if ".test." in path.name:
                continue
            for imported in re.findall(
                r'(?:from\s*|import\s*)["\']([^"\']+)["\']', path.read_text()
            ):
                if imported.startswith("."):
                    assert (path.parent / imported).resolve() not in transport, (path, imported)


def test_feature_worker_entries_do_not_import_runtime_configuration_or_transport():
    root = Path(__file__).resolve().parents[1] / "src" / "asterion"
    for domain in ("data", "research"):
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
                    "asterion.platform.secrets",
                }, (path, name)
