"""Backup validation and atomic restore mechanisms; domain operations belong to plugins."""

from collections.abc import Callable
from dataclasses import dataclass


@dataclass(frozen=True)
class BackupCheck[T]:
    contract: type[T]
    validate: Callable[[T], dict[str, int]]


def validate_checks(plugins, inputs):
    checks = {plugin.id: plugin.backup for plugin in plugins if plugin.backup is not None}
    if set(checks) != set(inputs):
        raise ValueError("Backup inputs do not match validators")
    for identifier, check in checks.items():
        if not isinstance(inputs[identifier], check.contract):
            raise TypeError(f"Invalid backup input: {identifier}")
    counts = {}
    for identifier, check in checks.items():
        result = check.validate(inputs[identifier])
        if not isinstance(result, dict) or any(
            not isinstance(key, str) or type(value) is not int or value < 0
            for key, value in result.items()
        ):
            raise ValueError("Invalid backup validation metrics")
        if counts.keys() & result.keys():
            raise ValueError("Duplicate backup validation metric")
        counts.update(result)
    return counts


@dataclass(frozen=True)
class RestoreStep[T]:
    contract: type[T]
    apply: Callable[[T], None]


class RestoreScope:
    """One-use operations valid only inside the core-owned restore transaction."""

    def __init__(self):
        self._active = True
        self._states = []

    def operation(self, action):
        if not self._active:
            raise ValueError("Restore scope is closed")
        state = ["pending"]
        self._states.append(state)

        def invoke():
            if not self._active:
                raise ValueError("Restore scope is closed")
            if state[0] != "pending":
                raise ValueError("Restore operation was already invoked")
            state[0] = "failed"
            action()
            state[0] = "complete"

        return invoke

    def verify(self):
        if any(state[0] != "complete" for state in self._states):
            raise ValueError("Required restore operations did not complete")

    def close(self):
        self._active = False


def isolate_restore(engine, plugins, bind_inputs):
    """Cancel unfinished tasks and apply required domain isolation in one transaction."""
    from sqlalchemy import update

    from asterion.platform.store import jobs

    scope = RestoreScope()
    try:
        with engine.begin() as conn:
            steps = {plugin.id: plugin.restore for plugin in plugins if plugin.restore is not None}
            inputs = bind_inputs(conn, plugins, scope)
            if set(steps) != set(inputs):
                raise ValueError("Restore inputs do not match steps")
            for identifier, step in steps.items():
                if not isinstance(inputs[identifier], step.contract):
                    raise TypeError(f"Invalid restore input: {identifier}")
            result = conn.execute(
                update(jobs)
                .where(jobs.c.state.in_(("QUEUED", "RUNNING")))
                .values(
                    state="CANCELLED",
                    token=None,
                    lease_until=None,
                    worker_id=None,
                    error="恢复后隔离：请检查原任务后显式重试",
                )
            )
            for identifier, step in steps.items():
                step.apply(inputs[identifier])
            scope.verify()
            return result.rowcount
    finally:
        scope.close()
