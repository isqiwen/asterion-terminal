import json
from pathlib import Path

from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.data.reference import ReferenceCatalog
from asterion.platform.config import Settings

if __name__ == "__main__":
    app = create_app(Settings(token="schema-generation-only-token"), create_engine("sqlite://"))
    Path("docs/openapi.json").write_text(
        json.dumps(app.openapi(), ensure_ascii=False, indent=2) + "\n"
    )

    Path("docs/reference-schema.json").write_text(
        json.dumps(ReferenceCatalog.model_json_schema(), ensure_ascii=False, indent=2) + "\n"
    )
