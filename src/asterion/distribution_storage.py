"""Explicit database ownership of the default distribution."""

from asterion_bindings.storage import Storage

from asterion.platform.store import jobs


def data_storage(engine):
    from asterion.data.catalog import snapshots
    from asterion.data.connections import (
        configurations,
        connection_settings,
        connections,
        verification_records,
    )
    from asterion.data.coverage import refills, reports
    from asterion.data.ingestion import observations
    from asterion.data.library import collections, versions
    from asterion.data.preparation import batches
    from asterion.data.reference_store import releases
    from asterion.data.version_state import version_states

    return Storage(
        engine,
        (
            snapshots,
            configurations,
            verification_records,
            connection_settings,
            connections,
            refills,
            reports,
            observations,
            collections,
            versions,
            batches,
            releases,
            version_states,
        ),
        read=(jobs,),
        filters={jobs: jobs.c.kind.in_(("data.sync", "data.import_csv"))},
    )


def research_storage(engine):
    from asterion.research.experiments import experiments
    from asterion.research.packages import packages
    from asterion.research.service import results
    from asterion.research.validation import validations
    from asterion.research.workspace import workspaces

    return Storage(
        engine,
        (packages, results, workspaces, experiments, validations),
        read=(jobs,),
        filters={jobs: jobs.c.kind == "research.backtest"},
    )


def rule_storage(engine):
    from asterion.contract_rules.service import rules

    return Storage(engine, (rules,))


def role_storage(engine):
    from asterion.contract_roles.plugin import computed_versions, versions
    from asterion.contract_roles.sync_workflow import workflows

    return Storage(
        engine,
        (versions, computed_versions, workflows),
        read=(jobs,),
        filters={jobs: jobs.c.kind == "contract_roles.continue"},
    )
