import argparse
import logging
from pathlib import Path

from asterion.platform.config import Settings


def main():
    parser = argparse.ArgumentParser(prog="asterion")
    parser.add_argument(
        "role",
        choices=[
            "init",
            "serve",
            "worker",
            "node",
            "desktop-bootstrap",
            "desktop-supervise",
            "desktop-stop",
        ],
    )
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--pg-root", type=Path)
    args = parser.parse_args()
    if args.role.startswith("desktop-"):
        import json

        from asterion.runtime.desktop import bootstrap, stop, supervise

        if args.state is None:
            parser.error("--state is required")
        if args.role == "desktop-stop":
            stop(args.state)
        else:
            if args.pg_root is None:
                parser.error("--pg-root is required")
            if args.role == "desktop-bootstrap":
                print(json.dumps(bootstrap(args.state, args.pg_root)))
            else:
                supervise(args.state, args.pg_root)
        return
    settings = Settings()
    logging.basicConfig(level=logging.INFO)
    if args.role == "init":
        from asterion.platform.store import database, metadata

        settings.require_token()
        from asterion.data.public import initialize_catalog

        engine = database(settings.database_url)
        metadata.create_all(engine)
        initialize_catalog(engine)
        for directory in ("sources", "published", "artifacts", "backups"):
            (settings.data_root / directory).mkdir(parents=True, exist_ok=True)
        print("Catalog and local storage initialized")
    elif args.role == "serve":
        import uvicorn

        from asterion.api.app import create_app

        uvicorn.run(create_app(settings), host="127.0.0.1", port=args.port)
    elif args.role == "worker":
        from asterion.runtime.worker import run

        run(settings, args.once)
    else:
        parser.error("Trading node is not implemented. No broker connection or orders are enabled.")


if __name__ == "__main__":
    main()
