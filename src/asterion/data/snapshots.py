import hashlib
import os
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
                if old["manifest"]["checksum"] != digest:
                    raise Conflict("Publication retry changed content")
                return dict(old)
            job = self.tasks.require_lease(conn, job_id, token)
            # Validate the result against the authoritative input, not worker-supplied metadata.
            from asterion.data.public import encode_csv

            expected, manifest = encode_csv(job["payload"]["csv"])
            import csv
            import io

            from asterion.data.sync import atomic_write

            self.library.types.get("futures.bars").validate(
                list(csv.DictReader(io.StringIO(job["payload"]["csv"])))
            )
            if expected != content:
                raise ValueError("Worker result does not match admitted input")
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
            }
            record = {"id": snapshot_id, "job_id": job_id, "manifest": manifest}
            self.tasks.require_lease(conn, job_id, token)
            raw_path = Path("imports") / snapshot_id / "source.csv"
            atomic_write(self.root / raw_path, job["payload"]["csv"].encode())
            self.library.publish_pair(
                conn,
                job_id=job_id,
                type_id="futures.bars",
                source="local_file",
                scope={"name": job["payload"]["source"], "contracts": manifest["contracts"]},
                raw_path=str(raw_path),
                raw_format="csv",
                standard_path=str(path.relative_to(self.root)),
                row_count=manifest["rows"],
                snapshot_id=snapshot_id,
                detail={
                    "first": manifest["start"],
                    "last": manifest["end"],
                    "coverage": "IMPORTED_ROWS_ONLY",
                    "empty_partitions": [],
                },
            )
            conn.execute(snapshots.insert().values(**record))
            self.tasks.require_lease(conn, job_id, token)
            conn.execute(
                jobs.update()
                .where(jobs.c.id == job_id)
                .values(state="SUCCEEDED", result={"snapshot_id": snapshot_id})
            )
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
