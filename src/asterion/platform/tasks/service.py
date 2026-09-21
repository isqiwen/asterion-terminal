import time
from uuid import uuid4

from sqlalchemy import and_, or_, select, update
from sqlalchemy.exc import IntegrityError

from asterion.platform.store import jobs


class Conflict(ValueError):
    pass


class Tasks:
    def __init__(self, engine, lease_seconds=60):
        self.engine = engine
        self.lease_seconds = lease_seconds

    @staticmethod
    def record(command_id, kind, payload):
        return {
            "id": str(uuid4()),
            "command_id": command_id,
            "kind": kind,
            "payload": payload,
            "state": "QUEUED",
            "attempt": 0,
            "created_at": time.time(),
        }

    def submit_batch(self, conn, commands):
        records = [self.record(command_id, kind, payload) for command_id, kind, payload in commands]
        if records:
            conn.execute(jobs.insert(), records)
        return records

    def submit(self, command_id, kind, payload):
        record = self.record(command_id, kind, payload)
        try:
            with self.engine.begin() as conn:
                conn.execute(jobs.insert().values(**record))
        except IntegrityError:
            with self.engine.connect() as conn:
                old = (
                    conn.execute(select(jobs).where(jobs.c.command_id == command_id))
                    .mappings()
                    .one()
                )
                if old["payload"] != payload or old["kind"] != kind:
                    raise Conflict("command_id reused with different input")
                return dict(old)
        return record

    def list(self):
        with self.engine.connect() as conn:
            return [
                dict(r)
                for r in conn.execute(
                    select(
                        *(column for column in jobs.c if column.name not in {"payload", "token"})
                    )
                    .order_by(jobs.c.created_at.desc())
                    .limit(100)
                ).mappings()
            ]

    def get(self, job_id):
        with self.engine.connect() as conn:
            row = (
                conn.execute(
                    select(*(c for c in jobs.c if c.name not in {"payload", "token"})).where(
                        jobs.c.id == job_id
                    )
                )
                .mappings()
                .first()
            )
        if row is None:
            raise KeyError(job_id)
        return dict(row)

    def claim(self, worker_id):
        now = time.time()
        eligible = or_(
            jobs.c.state == "QUEUED", and_(jobs.c.state == "RUNNING", jobs.c.lease_until < now)
        )
        with self.engine.begin() as conn:
            row = (
                conn.execute(
                    select(jobs)
                    .where(eligible)
                    .order_by(jobs.c.created_at)
                    .with_for_update(skip_locked=True)
                    .limit(1)
                )
                .mappings()
                .first()
            )
            if row is None:
                return None
            values = {
                "state": "RUNNING",
                "token": str(uuid4()),
                "attempt": row["attempt"] + 1,
                "worker_id": worker_id,
                "lease_until": now + self.lease_seconds,
                "error": None,
            }
            changed = conn.execute(
                update(jobs)
                .where(jobs.c.id == row["id"], eligible, jobs.c.attempt == row["attempt"])
                .values(**values)
            )
            if changed.rowcount != 1:
                return None
            return dict(row) | values

    def require_lease(self, conn, job_id, token):
        row = (
            conn.execute(select(jobs).where(jobs.c.id == job_id).with_for_update())
            .mappings()
            .first()
        )
        if (
            not row
            or row["state"] != "RUNNING"
            or row["token"] != token
            or row["lease_until"] <= time.time()
        ):
            raise Conflict("Lease expired, cancelled, or superseded")
        return row

    def complete(self, conn, job_id, token, result):
        self.require_lease(conn, job_id, token)
        conn.execute(
            jobs.update().where(jobs.c.id == job_id).values(state="SUCCEEDED", result=result)
        )

    def progress(self, conn, job_id, token, result):
        self.require_lease(conn, job_id, token)
        conn.execute(jobs.update().where(jobs.c.id == job_id).values(result=result))

    def heartbeat(self, job_id, token):
        with self.engine.begin() as conn:
            self.require_lease(conn, job_id, token)
            conn.execute(
                jobs.update()
                .where(jobs.c.id == job_id)
                .values(lease_until=time.time() + self.lease_seconds)
            )

    def fail(self, job_id, token, error):
        with self.engine.begin() as conn:
            self.require_lease(conn, job_id, token)
            conn.execute(
                jobs.update().where(jobs.c.id == job_id).values(state="FAILED", error=error[:2000])
            )

    @staticmethod
    def cancel_batch(conn, job_ids):
        changed = conn.execute(
            jobs.update()
            .where(jobs.c.id.in_(job_ids), jobs.c.state.in_(["QUEUED", "RUNNING"]))
            .values(state="CANCELLED")
        )
        return changed.rowcount

    def cancel(self, job_id):
        with self.engine.begin() as conn:
            if self.cancel_batch(conn, [job_id]) != 1:
                raise Conflict("Job is missing or already terminal")
