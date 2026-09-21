"""Read-only dependency reporting without disclosing account-owned research contents."""

from sqlalchemy import select

from asterion.platform.store import jobs
from asterion.research.packages import packages
from asterion.research.parameters import (
    BooleanParameter,
    Choice,
    DecimalParameter,
    EnumParameter,
    IntegerParameter,
)
from asterion.research.strategies import ClosedBar, Strategy, StrategyInfo, StrategyRef
from asterion.research.workspace import workspaces

__all__ = [
    "BooleanParameter",
    "Choice",
    "ClosedBar",
    "DecimalParameter",
    "EnumParameter",
    "IntegerParameter",
    "Strategy",
    "StrategyInfo",
    "StrategyRef",
    "version_references",
]


def _rule_versions(spec):
    return {
        (spec.get("basis") or {}).get("version_id"),
        ((spec.get("contract") or {}).get("provenance") or {}).get("source_version"),
        *{
            (p.get("settlement_basis") or {}).get("evidence", {}).get("version_id")
            for p in spec.get("periods", [])
        },
    }


def _input_versions(payload):
    coverage = payload.get("coverage") or {}
    version = payload.get("version") or {}
    return {
        (payload.get("request") or {}).get("version_id"),
        version.get("id"),
        *_rule_versions(((payload.get("request") or {}).get("rules") or {}).get("spec", {})),
        coverage.get("daily_version_id"),
        coverage.get("calendar_version_id"),
        coverage.get("contracts_version_id"),
        *(version.get("manifest") or {}).get("inputs", []),
    }


def version_references(conn, version_id):
    """Count all owners, including queued/failed runs, without exposing names or IDs."""
    counts = {"research_runs": 0, "research_documents": 0, "research_packages": 0}
    for payload in conn.execute(
        select(jobs.c.payload).where(jobs.c.kind == "research.backtest")
    ).scalars():
        counts["research_runs"] += version_id in _input_versions(payload)
    for content in conn.execute(
        select(workspaces.c.content).where(workspaces.c.deleted.is_(False))
    ).scalars():
        config = content.get("config") or {}
        counts["research_documents"] += version_id in {
            config.get("version_id"),
            *_rule_versions((config.get("rules") or {}).get("spec", {})),
        }
    for value in conn.execute(select(packages.c.package)).scalars():
        counts["research_packages"] += version_id in _input_versions(value["content"])
    return counts
