import json
from pathlib import Path
from tempfile import TemporaryDirectory

from asterion_bindings.catalog import ReferenceCatalog
from asterion_bindings.database import create_engine

from asterion.api.app import create_app
from asterion.platform.config import Settings

NATIVE = Path("services/server/openapi.json")


def native():
    """The published description of the operations the Rust entry implements."""
    return json.loads(NATIVE.read_text())


def references(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "$ref":
                yield item
            else:
                yield from references(item)
    elif isinstance(value, list):
        for item in value:
            yield from references(item)


def merged(internal, entry):
    """One API description: every operation has exactly one owner, and a schema
    name shared by both owners must mean exactly the same contract."""
    paths = {path: dict(item) for path, item in internal["paths"].items()}
    for path, item in entry["paths"].items():
        for method, operation in item.items():
            if method in paths.get(path, {}):
                raise SystemExit(f"{method.upper()} {path} is described by both owners")
            paths.setdefault(path, {})[method] = operation
    schemas = dict(internal["components"]["schemas"])
    for name, schema in entry["components"]["schemas"].items():
        if name in schemas and schemas[name] != schema:
            raise SystemExit(f"Schema {name} differs between the entry and the internal process")
        schemas[name] = schema
    document = {
        **internal,
        "paths": dict(sorted(paths.items())),
        "components": {**internal["components"], "schemas": dict(sorted(schemas.items()))},
    }
    for reference in references(document):
        name = reference.removeprefix("#/components/schemas/")
        if name not in schemas:
            raise SystemExit(f"Unresolved schema reference {reference}")
    return document


if __name__ == "__main__":
    with TemporaryDirectory(prefix="asterion-schema-") as directory:
        engine = create_engine("sqlite://")
        app = create_app(
            Settings(token="schema-generation-only-token", data_root=Path(directory)), engine
        )
        try:
            schema = app.openapi()
            Path("docs/openapi.json").write_text(
                json.dumps(merged(schema, native()), ensure_ascii=False, indent=2) + "\n"
            )
            # Operations still owned by the internal Python process; the Rust
            # entry forwards exactly these and nothing else.
            routes = [
                {"method": method, "path": path}
                for path, method in sorted(
                    (path, method.upper())
                    for path, operations in schema["paths"].items()
                    for method in operations
                )
            ]
            Path("services/server/forwarded_routes.json").write_text(
                json.dumps(routes, indent=1) + "\n"
            )
            # Scope policies, worker grants and event read paths the entry
            # authorizes with; declared by the Python assembly during migration.
            Path("services/server/authorization.json").write_text(
                json.dumps(app.state.entry_authorization, indent=1, sort_keys=True) + "\n"
            )
        finally:
            app.state.plugins.close()
            engine.dispose()

    Path("docs/reference-schema.json").write_text(
        json.dumps(ReferenceCatalog.model_json_schema(), ensure_ascii=False, indent=2) + "\n"
    )
