"""Validate rule identities in restored storage without mutating evidence."""

from collections.abc import Callable, Iterator
from dataclasses import dataclass

from sqlalchemy import text

from asterion.contract_rules.public import RuleVersion
from asterion.platform.backup import BackupCheck


@dataclass(frozen=True)
class RuleBackup:
    versions: Callable[[], Iterator[dict]]


def load_evidence(conn):
    def versions():
        for row in conn.execute(text("SELECT id, spec FROM contract_rule_versions")).mappings():
            yield dict(row)

    return RuleBackup(versions)


def validate(evidence: RuleBackup):
    count = 0
    for value in evidence.versions():
        RuleVersion.model_validate(value)
        count += 1
    return {"contract_rule_versions": count}


check = BackupCheck(RuleBackup, validate)
