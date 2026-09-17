import ast
from pathlib import Path


def test_business_modules_only_import_other_public_interfaces():
    root = Path(__file__).resolve().parents[1] / "src" / "asterion"
    modules = {"data", "research", "trading", "intelligence"}
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
