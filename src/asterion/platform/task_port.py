"""Task operations for approved functionality; worker scheduling stays in the host."""

from collections.abc import Callable
from dataclasses import dataclass

from sqlalchemy import select

from asterion.platform.storage import transaction_connection
from asterion.platform.store import jobs
from asterion.platform.tasks.service import Tasks


@dataclass(frozen=True)
class TaskPort:
    submit: Callable
    submit_batch: Callable
    require_lease: Callable
    complete: Callable
    progress: Callable
    cancel_batch: Callable


def task_port(tasks: Tasks, kinds: frozenset[str]) -> TaskPort:
    def connection(conn):
        value = transaction_connection(conn)
        if value.engine is not tasks.engine:
            raise ValueError("Task transaction belongs to another database")
        return value

    def check(kind):
        if kind not in kinds:
            raise ValueError(f"Task kind is not granted: {kind}")

    def submit(command_id, kind, payload):
        check(kind)
        return tasks.submit(command_id, kind, payload)

    def submit_batch(conn, commands):
        commands = list(commands)
        for _, kind, _ in commands:
            check(kind)
        return tasks.submit_batch(connection(conn), commands)

    def require_lease(conn, job_id, token):
        row = tasks.require_lease(connection(conn), job_id, token)
        check(row["kind"])
        return row

    def complete(conn, job_id, token, result):
        require_lease(conn, job_id, token)
        tasks.complete(connection(conn), job_id, token, result)

    def progress(conn, job_id, token, result):
        require_lease(conn, job_id, token)
        tasks.progress(connection(conn), job_id, token, result)

    def cancel_batch(conn, identifiers):
        identifiers = set(identifiers)
        raw = connection(conn)
        rows = raw.execute(
            select(jobs.c.id, jobs.c.kind).where(jobs.c.id.in_(identifiers)).with_for_update()
        ).all()
        if len(rows) != len(identifiers):
            raise ValueError("Task not found")
        for row in rows:
            check(row.kind)
        return tasks.cancel_batch(raw, identifiers)

    return TaskPort(submit, submit_batch, require_lease, complete, progress, cancel_batch)
