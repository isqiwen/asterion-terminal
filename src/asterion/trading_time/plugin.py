"""Offline backup check of trading-time versions.

Versions, their storage and the trading-time operations belong to the Rust
entry. Backup validation still runs in Python against the isolated restore
database, so the evidence check stays here until recovery moves to Rust.
"""

import json

from asterion_bindings.calendar import TimeVersion
from asterion_bindings.plugin_host import Activation, Plugin
from asterion_bindings.recovery import BackupCheck
from sqlalchemy import text


def load_evidence(conn):
    rows = conn.execute(text("SELECT id, spec FROM trading_time_versions")).all()
    return [
        {"id": row[0], "spec": json.loads(row[1]) if isinstance(row[1], str) else row[1]}
        for row in rows
    ]


def validate(evidence: list):
    for value in evidence:
        TimeVersion.model_validate(value)
    return {"trading_time_versions": len(evidence)}


plugin = Plugin(
    "asterion.trading_time",
    (),
    lambda _: Activation(),
    backup=BackupCheck(list, validate),
)
