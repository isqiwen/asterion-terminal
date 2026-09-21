"""Read-only local F2 evidence inspection and consecutive publication verification."""

import argparse
import json
import sys
from pathlib import Path

from sqlalchemy import create_engine, select

from asterion.contract_roles.computed import ComputedSources
from asterion.contract_roles.computed_public import ComputedVersion
from asterion.contract_roles.plugin import computed_versions
from asterion.contract_roles.sequence import SequenceRequest, verify_sequence
from asterion.data.public import snapshot_backup_access
from asterion.platform.files import read_files
from asterion.runtime.desktop import runtime_settings
from asterion.runtime.environments import active


def inspect_evidence(connection, files, request: SequenceRequest | None):
    records = {
        row["id"]: ComputedVersion.model_validate(dict(row))
        for row in connection.execute(select(computed_versions)).mappings()
    }
    if request is None:
        return {
            "status": "NOT_VERIFIED",
            "available_versions": [
                {
                    "id": record.id,
                    "published_at": record.published_at.isoformat(),
                    "observation_day": str(record.spec.input.observations[-1].trading_day),
                }
                for record in sorted(records.values(), key=lambda item: item.published_at)
            ],
            "next_action": "Select an ordered publication chain, or publish its first day.",
        }
    missing = set(request.version_ids) - records.keys()
    if missing:
        raise ValueError("Requested published versions are missing")
    sources = ComputedSources(snapshot_backup_access(connection, files))
    result = verify_sequence(sources, records.__getitem__, request)
    return {
        "status": "VERIFIED",
        "scope": "local_role_publication_evidence",
        "minimum_switches": request.minimum_switches,
        "result": result.model_dump(mode="json"),
        "live_collection_attested": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default = Path.home() / (
        ".local/share/me.asterion.terminal"
        if sys.platform == "linux"
        else "Library/Application Support/me.asterion.terminal"
    )
    parser.add_argument("--host", type=Path, default=default)
    parser.add_argument("--version", action="append", help="Repeat in publication order")
    parser.add_argument("--minimum-switches", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    engine = None
    try:
        host, output = args.host.resolve(), args.output.resolve()
        state = active(host)
        if any(output.is_relative_to(path) for path in (host, state)):
            raise ValueError("Report must be outside the active environment")
        # Refuse overwriting evidence, and never serialize credentials or raw exceptions.
        with output.open("x", encoding="utf-8") as report:
            try:
                request = (
                    SequenceRequest(
                        version_ids=args.version, minimum_switches=args.minimum_switches
                    )
                    if args.version
                    else None
                )
                settings = runtime_settings(state, json.loads((state / "desktop.json").read_text()))
                engine = create_engine(
                    settings.database_url, hide_parameters=True, connect_args={"connect_timeout": 5}
                )
                with engine.connect() as connection:
                    connection.exec_driver_sql(
                        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY"
                    )
                    result = inspect_evidence(connection, read_files(settings.data_root), request)
                json.dump(result, report, ensure_ascii=False, indent=2)
            except Exception as error:
                json.dump({"status": "FAILED", "error_type": type(error).__name__}, report)
                raise
        print(f"{result['status']}: {output}")
        return 0 if result["status"] == "VERIFIED" else 2
    except Exception as error:  # noqa: BLE001 - never expose credential-bearing exceptions
        print(f"Role evidence verification unavailable ({type(error).__name__}).")
        return 1
    finally:
        if engine is not None:
            engine.dispose()


if __name__ == "__main__":
    raise SystemExit(main())
