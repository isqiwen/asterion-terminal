"""Python contracts/callbacks; one-use handles and validation rules are native."""

from collections.abc import Callable
from dataclasses import dataclass

from . import _native
from ._native import RestoreScope

__all__ = ["BackupCheck", "RestoreScope", "RestoreStep", "restore_steps", "validate_checks"]


@dataclass(frozen=True)
class BackupCheck[T]:
    contract: type[T]
    validate: Callable[[T], dict[str, int]]


@dataclass(frozen=True)
class RestoreStep[T]:
    contract: type[T]
    apply: Callable[[T], None]


def validate_checks(plugins, inputs):
    checks = [(plugin.id, plugin.backup) for plugin in plugins if plugin.backup is not None]
    _native.recovery_inputs([identifier for identifier, _ in checks], list(inputs), False)
    for identifier, check in checks:
        if not isinstance(inputs[identifier], check.contract):
            raise TypeError(f"Invalid backup input: {identifier}")
    metrics = _native.BackupMetrics()
    for identifier, check in checks:
        metrics.add(check.validate(inputs[identifier]))
    return metrics.counts()


def restore_steps(plugins, inputs):
    steps = [(plugin.id, plugin.restore) for plugin in plugins if plugin.restore is not None]
    _native.recovery_inputs([identifier for identifier, _ in steps], list(inputs), True)
    for identifier, step in steps:
        if not isinstance(inputs[identifier], step.contract):
            raise TypeError(f"Invalid restore input: {identifier}")
    return steps
