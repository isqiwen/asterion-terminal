"""Build boundaries for migrated Rust modules, independent of activation order."""

import ast
import json
import tomllib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DOMAIN_EDGES = {
    "asterion-instrument-catalog": set(),
    "asterion-trading-calendar": {"asterion-instrument-catalog"},
    "asterion-market-rules": {"asterion-instrument-catalog", "asterion-trading-calendar"},
    "asterion-connections": set(),
    "asterion-data-store": {"asterion-instrument-catalog", "asterion-trading-calendar"},
    "asterion-market-feed": {
        "asterion-instrument-catalog",
        "asterion-trading-calendar",
        "asterion-connections",
    },
    "asterion-role-registry": {
        "asterion-instrument-catalog",
        "asterion-trading-calendar",
        "asterion-data-store",
    },
    "asterion-execution": {
        "asterion-instrument-catalog",
        "asterion-trading-calendar",
        "asterion-market-rules",
        "asterion-connections",
    },
}


def test_python_database_io_uses_the_fixed_native_driver():
    adapter = ROOT / "bindings/python/asterion_bindings/database.py"
    sources = [*(ROOT / "src").rglob("*.py"), *(ROOT / "bindings/python").rglob("*.py")]
    for source in sources:
        module = ast.parse(source.read_text())
        sqlalchemy_names = set()
        for node in ast.walk(module):
            if isinstance(node, ast.Import):
                for imported in node.names:
                    assert imported.name.split(".")[0] not in {"sqlite3", "psycopg", "psycopg2"}, (
                        f"Python database driver bypasses the kernel: {source}"
                    )
                    if imported.name.startswith("sqlalchemy"):
                        sqlalchemy_names.add(imported.asname or imported.name.split(".")[0])
            elif isinstance(node, ast.ImportFrom):
                assert (node.module or "").split(".")[0] not in {"sqlite3", "psycopg", "psycopg2"}
                if (node.module or "").startswith("sqlalchemy"):
                    assert source == adapter or not any(
                        imported.name in {"create_engine", "create_async_engine"}
                        for imported in node.names
                    ), f"Physical engines must be created by the native adapter: {source}"
        if source == adapter:
            continue
        for node in ast.walk(module):
            if not isinstance(node, ast.Attribute) or node.attr not in {
                "create_engine",
                "create_async_engine",
            }:
                continue
            value = node.value
            while isinstance(value, ast.Attribute):
                value = value.value
            assert not isinstance(value, ast.Name) or value.id not in sqlalchemy_names, (
                f"Physical engines must be created by the native adapter: {source}"
            )


def test_language_bindings_do_not_import_product_or_application_implementations():
    for source in (ROOT / "bindings/python/asterion_bindings").glob("*.py"):
        for node in ast.walk(ast.parse(source.read_text())):
            names = (
                [entry.name for entry in node.names]
                if isinstance(node, ast.Import)
                else [node.module or ""]
                if isinstance(node, ast.ImportFrom)
                else []
            )
            assert not any(name == "asterion" or name.startswith("asterion.") for name in names), (
                f"Language binding imports an application implementation: {source}"
            )


def validate_graph(layers, graph, kinds):
    visiting, visited = set(), set()

    def visit(name):
        assert name not in visiting, f"Build dependency cycle at {name}"
        if name in visited:
            return
        visiting.add(name)
        for dependency in graph[name]:
            assert layers[dependency] <= layers[name], f"Upward dependency: {name} -> {dependency}"
            if layers[name] <= 1:
                assert layers[dependency] <= 1, f"Kernel mechanism depends on {dependency}"
            if layers[name] == 4:
                assert layers[dependency] != 2, (
                    f"UI bypasses application boundary: {name} -> {dependency}"
                )
            if kinds[name] == "presentation":
                assert kinds[dependency] != "panel", (
                    f"Shared presentation depends on business UI: {name} -> {dependency}"
                )
            if layers[name] == layers[dependency] == 2:
                assert dependency in DOMAIN_EDGES[name], (
                    f"Undeclared domain dependency: {name} -> {dependency}"
                )
            visit(dependency)
        visiting.remove(name)
        visited.add(name)

    for name in graph:
        visit(name)


def test_rust_workspace_owns_one_layer_per_module_and_no_upward_dependencies():
    workspace = tomllib.loads((ROOT / "Cargo.toml").read_text())["workspace"]
    manifests = {}
    # The Tauri launcher is intentionally an independent Cargo workspace, but
    # still belongs to this product's architectural dependency graph.
    members = [*workspace["members"], "apps/terminal/src-tauri"]
    for member in members:
        directory = ROOT / member
        data = tomllib.loads((directory / "Cargo.toml").read_text())
        package = data["package"]
        metadata = package["metadata"]["asterion"]
        layer = int(metadata["layer"].removeprefix("L"))
        assert 0 <= layer <= 5
        if layer <= 1:
            assert metadata["kind"] == {0: "fixed", 1: "mechanism"}[layer]
            assert member == {0: "kernel/foundation", 1: "kernel/mechanisms"}[layer]
        if layer <= 2:
            assert not (directory / "manifest.json").exists()
        package_json = directory / "package.json"
        if package_json.is_file():
            frontend = json.loads(package_json.read_text())["asterion"]
            assert frontend["layer"] == layer, f"Mixed Rust/TypeScript layers in {member}"
        if layer == 3:
            # Rust application services, and the Rust side of the Python bindings.
            assert metadata["kind"] in {"service", "adapter", "binding"}
            assert member.startswith(
                {"service": "services/", "adapter": "adapters/", "binding": "bindings/"}[
                    metadata["kind"]
                ]
            )
        if layer == 4:
            assert metadata["kind"] in {"presentation", "panel"}
        if layer == 5:
            assert metadata["kind"] in {"product", "entry"}
        if layer == 2:
            # The futures domain is fixed kernel: no plugin manifest or identity.
            assert metadata["kind"] == "domain"
            assert member.startswith("kernel/futures/")
        if layer <= 2:
            assert not any(directory.rglob("*.py"))
            assert not any(directory.rglob("*.ts"))
            assert not any(directory.rglob("*.tsx"))
        manifests[package["name"]] = data
    layers = {
        name: int(data["package"]["metadata"]["asterion"]["layer"][1:])
        for name, data in manifests.items()
    }
    kinds = {
        name: data["package"]["metadata"]["asterion"]["kind"] for name, data in manifests.items()
    }
    graph = {}
    for name, data in manifests.items():
        dependencies = data.get("dependencies", {}) | data.get("build-dependencies", {})
        for target in data.get("target", {}).values():
            dependencies |= target.get("dependencies", {}) | target.get("build-dependencies", {})
        declared = set()
        for dependency, config in dependencies.items():
            if not isinstance(config, dict):
                continue
            definition = (
                workspace["dependencies"].get(dependency, {}) if config.get("workspace") else config
            )
            if not isinstance(definition, dict):
                definition = {}
            actual = config.get("package", definition.get("package", dependency))
            if actual in manifests:
                declared.add(actual)
        graph[name] = declared
    validate_graph(layers, graph, kinds)


@pytest.mark.parametrize(
    "layers,graph",
    [
        ({"kernel": 1, "business": 2}, {"kernel": {"business"}, "business": set()}),
        ({"a": 3, "b": 3}, {"a": {"b"}, "b": {"a"}}),
        (
            {"asterion-instrument-catalog": 2, "asterion-trading-calendar": 2},
            {
                "asterion-instrument-catalog": {"asterion-trading-calendar"},
                "asterion-trading-calendar": set(),
            },
        ),
    ],
)
def test_illegal_build_dependencies_are_rejected(layers, graph):
    with pytest.raises(AssertionError):
        validate_graph(layers, graph, {name: "domain" for name in layers})


@pytest.mark.parametrize(
    "layers,graph,kinds",
    [
        (
            {"shared": 4, "screen": 4},
            {"shared": {"screen"}, "screen": set()},
            {"shared": "presentation", "screen": "panel"},
        ),
        (
            {"screen": 4, "domain": 2},
            {"screen": {"domain"}, "domain": set()},
            {"screen": "panel", "domain": "domain"},
        ),
    ],
)
def test_presentation_cannot_reverse_own_business_ui_or_bypass_applications(layers, graph, kinds):
    with pytest.raises(AssertionError):
        validate_graph(layers, graph, kinds)
