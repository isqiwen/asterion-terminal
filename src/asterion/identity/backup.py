from collections.abc import Callable
from dataclasses import dataclass

from sqlalchemy import text

from asterion.platform.backup import BackupCheck, RestoreStep


@dataclass(frozen=True)
class IdentityBackup:
    accounts: int


def load_evidence(conn):
    return IdentityBackup(conn.execute(text("SELECT count(*) FROM identity_accounts")).scalar_one())


def validate_backup(evidence: IdentityBackup):
    return {"accounts": evidence.accounts}


check = BackupCheck(IdentityBackup, validate_backup)


@dataclass(frozen=True)
class SessionReset:
    clear: Callable[[], None]


def restore_inputs(conn, scope):
    return SessionReset(
        scope.operation(lambda: conn.execute(text("DELETE FROM identity_sessions")))
    )


def reset_sessions(operation: SessionReset):
    operation.clear()


restore = RestoreStep(SessionReset, reset_sessions)
