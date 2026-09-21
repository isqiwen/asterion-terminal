import hashlib
import os
from datetime import UTC, date, datetime
from pathlib import Path
from uuid import uuid4

from sqlalchemy import select

from asterion.data.catalog import snapshots
from asterion.data.library import DataLibrary
from asterion.platform.store import jobs
from asterion.platform.tasks.service import Conflict


class Snapshots:
    def __init__(self, engine, tasks, root: Path):
        self.engine, self.tasks, self.root = engine, tasks, root
        self.library = DataLibrary(engine, root)
        (root / "published").mkdir(parents=True, exist_ok=True)

    def publish(self, job_id, token, content):
        digest = hashlib.sha256(content).hexdigest()
        with self.engine.begin() as conn:
            job = (
                conn.execute(select(jobs).where(jobs.c.id == job_id).with_for_update())
                .mappings()
                .first()
            )
            if job and job["state"] == "SUCCEEDED" and job["token"] == token:
                old = (
                    conn.execute(select(snapshots).where(snapshots.c.job_id == job_id))
                    .mappings()
                    .one()
                )
                if old["manifest"]["import_checksum"] != digest:
                    raise Conflict("Publication retry changed content")
                return dict(old)
            job = self.tasks.require_lease(conn, job_id, token)
            # Validate the result against the authoritative input, not worker-supplied metadata.
            from asterion.data.artifacts import atomic_write
            from asterion.data.importing import encode_import, mapped
            from asterion.data.public import ImportOptions

            expected, manifest = encode_import(job["payload"])
            options = ImportOptions.model_validate(job["payload"]["options"])
            options.identity.validate_inputs(self.library.preview)
            type_id = options.type_id
            import_manifest = dict(manifest)
            standard_content = content
            if type_id == "futures.daily":
                chart = self.library.types.get(type_id).chart
                assert chart is not None
                content, manifest = chart(
                    mapped(job["payload"]["csv"], options),
                    datetime.fromtimestamp(job["created_at"], UTC).isoformat(),
                )
            if expected != standard_content:
                raise ValueError("Worker result does not match admitted input")
            digest = hashlib.sha256(content).hexdigest()
            snapshot_id = str(uuid4())
            path = self.root / "published" / f"{snapshot_id}.parquet"
            with path.open("xb") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise ValueError("Stored checksum mismatch")
            manifest |= {
                "uri": f"asterion://local/published/{snapshot_id}",
                "source": job["payload"]["source"],
                "state": "PUBLISHED",
                "import_checksum": hashlib.sha256(standard_content).hexdigest(),
            }
            record = {"id": snapshot_id, "job_id": job_id, "manifest": manifest}
            self.tasks.require_lease(conn, job_id, token)
            raw_path = Path("imports") / snapshot_id / "source.csv"
            atomic_write(self.root / raw_path, job["payload"]["csv"].encode())
            standard_path = path
            if type_id == "futures.daily":
                standard_path = self.root / "imports" / snapshot_id / "standard.parquet"
                atomic_write(standard_path, standard_content)
            scope = {
                "contract_ids": sorted(
                    {
                        options.identity.resolve(
                            row["contract"], date.fromisoformat(row["trading_day"])
                        ).id
                        for row in mapped(job["payload"]["csv"], options)
                    }
                ),
                "source_id": options.source_id,
                "contracts": manifest["contracts"],
                "frequency": options.frequency,
            }
            if type_id == "futures.daily":
                exchange, symbol = manifest["contracts"][0].split(".")
                scope.update(exchange=exchange, symbol=symbol)
            self.library.publish_pair(
                conn,
                job_id=job_id,
                type_id=type_id,
                source="local_file",
                scope=scope,
                raw_path=str(raw_path),
                raw_format="csv",
                standard_path=str(standard_path.relative_to(self.root)),
                row_count=manifest["rows"],
                snapshot_id=snapshot_id,
                detail={
                    "demo": manifest["demo"],
                    "origin": {
                        "method": "file",
                        "name": job["payload"]["source"],
                        "contract_ids": sorted(
                            {
                                options.identity.resolve(
                                    row["contract"], date.fromisoformat(row["trading_day"])
                                ).id
                                for row in mapped(job["payload"]["csv"], options)
                            }
                        ),
                        "source_id": options.source_id,
                    },
                    "import_options": options.model_dump(mode="json"),
                    "first": import_manifest["start"],
                    "last": import_manifest["end"],
                    "coverage": "IMPORTED_ROWS_ONLY",
                    "empty_partitions": [],
                },
            )
            conn.execute(snapshots.insert().values(**record))
            self.tasks.require_lease(conn, job_id, token)
            self.tasks.complete(conn, job_id, token, {"snapshot_id": snapshot_id})
            return record

    def list(self):
        with self.engine.connect() as conn:
            return [dict(r) for r in conn.execute(select(snapshots).limit(100)).mappings()]

    def path(self, snapshot_id):
        with self.engine.connect() as conn:
            record = conn.execute(select(snapshots).where(snapshots.c.id == snapshot_id)).first()
        if not record:
            raise KeyError(snapshot_id)
        return self.root / "published" / f"{snapshot_id}.parquet"
