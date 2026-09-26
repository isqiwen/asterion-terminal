"""Host transaction assembly for native task persistence and restricted ports."""

import json
import time
from collections.abc import Callable
from contextlib import contextmanager
from dataclasses import dataclass
from uuid import uuid4

from . import _native
from .communication import context, continuation, current
from .database import database_identity, kernel_call, native_connection
from .events import descriptor
from .storage import transaction_connection
from .task_models import TASK_CHANGED
from .tasks import task_rule

Conflict = _native.TaskConflict


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def record(command_id, kind, payload):
    return {
        "id": str(uuid4()),
        "command_id": command_id,
        "kind": kind,
        "payload": payload,
        "now": time.time(),
    }


class Tasks:
    def __init__(self, engine, lease_seconds=60):
        task_rule("duration", lease_seconds=lease_seconds)
        self.engine = engine
        self.lease_seconds = lease_seconds
        self._repository = _native.TaskRepository(
            database_identity(engine), encoded(descriptor(TASK_CHANGED))
        )

    @contextmanager
    def transaction(self, *, write=True):
        with self.engine.connect() as conn:
            if write:
                kernel_call(conn, native_connection(conn).begin_write)
            with conn.begin():
                yield conn

    def _run(self, conn, operation, **values):
        if conn.engine is not self.engine or not conn.in_transaction():
            raise ValueError("Task requires its database transaction")
        return json.loads(
            kernel_call(
                conn,
                self._repository.run,
                native_connection(conn),
                encoded({"op": operation, **values}),
                encoded(current() or context()),
            )
        )

    def submit(self, command_id, kind, payload):
        with self.transaction() as conn:
            return self._run(conn, "submit", record=record(command_id, kind, payload))

    def submit_batch(self, conn, commands):
        return self._run(conn, "submit_batch", records=[record(*command) for command in commands])

    def list(self):
        with self.transaction(write=False) as conn:
            return self._run(conn, "list")

    def get(self, job_id):
        with self.transaction(write=False) as conn:
            row = self._run(conn, "get", id=job_id)
        if row is None:
            raise KeyError(job_id)
        return row

    def claim(self, worker_id):
        with self.transaction() as conn:
            row = self._run(
                conn,
                "claim",
                worker_id=worker_id,
                now=time.time(),
                lease_seconds=self.lease_seconds,
                token=str(uuid4()),
            )
            if row is not None:
                event = row.pop("communication_event")
                row["communication"] = continuation(event["correlation_id"], event["id"], 86400)
            return row

    def require_lease(self, conn, job_id, token):
        return self._run(conn, "lease", id=job_id, token=token, now=time.time())

    def _apply(self, conn, job_id, action):
        return self._run(conn, "apply", id=job_id, action=action, now=time.time())

    def complete(self, conn, job_id, token, result):
        return self._apply(conn, job_id, {"type": "complete", "token": token, "result": result})

    def progress(self, conn, job_id, token, result):
        return self._apply(conn, job_id, {"type": "progress", "token": token, "result": result})

    def heartbeat(self, job_id, token):
        with self.transaction() as conn:
            return self._apply(
                conn,
                job_id,
                {"type": "heartbeat", "token": token, "lease_seconds": self.lease_seconds},
            )

    def fail(self, job_id, token, error):
        with self.transaction() as conn:
            return self._apply(conn, job_id, {"type": "fail", "token": token, "error": error})

    def cancel_batch(self, conn, identifiers):
        return self._run(conn, "cancel_batch", ids=sorted(set(identifiers)))

    def cancel(self, job_id):
        with self.transaction() as conn:
            if self.cancel_batch(conn, [job_id]) != 1:
                raise Conflict("Job is missing or already terminal")

    def isolate(self, conn, reason):
        return self._run(conn, "isolate", reason=reason)


@dataclass(frozen=True)
class TaskPort:
    submit: Callable
    submit_batch: Callable
    require_lease: Callable
    complete: Callable
    progress: Callable
    cancel_batch: Callable


def task_port(tasks, kinds):
    grant = tasks._repository.granted(sorted(kinds))

    def run(transaction, operation, **values):
        connection = transaction_connection(transaction, write=True)
        return json.loads(
            kernel_call(
                connection,
                grant.run,
                transaction._handle,
                encoded({"op": operation, **values}),
                encoded(current() or context()),
            )
        )

    def submit(command_id, kind, payload):
        with tasks.transaction() as conn:
            return json.loads(
                kernel_call(
                    conn,
                    grant.submit,
                    native_connection(conn),
                    encoded(record(command_id, kind, payload)),
                    encoded(current() or context()),
                )
            )

    def submit_batch(transaction, commands):
        return run(transaction, "submit_batch", records=[record(*command) for command in commands])

    def lease(transaction, job_id, token):
        return run(transaction, "lease", id=job_id, token=token, now=time.time())

    def complete(transaction, job_id, token, result):
        return run(
            transaction,
            "apply",
            id=job_id,
            action={"type": "complete", "token": token, "result": result},
            now=time.time(),
        )

    def progress(transaction, job_id, token, result):
        return run(
            transaction,
            "apply",
            id=job_id,
            action={"type": "progress", "token": token, "result": result},
            now=time.time(),
        )

    def cancel_batch(transaction, identifiers):
        return run(transaction, "cancel_batch", ids=sorted(set(identifiers)))

    return TaskPort(submit, submit_batch, lease, complete, progress, cancel_batch)
