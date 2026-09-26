"""Test-owned assembly: retain raw engines for corruption fixtures and scheduler assertions."""

from asterion_bindings.storage import Storage
from asterion_bindings.task_repository import Tasks

from asterion.distribution_storage import data_storage, research_storage
from asterion.platform.communication.schema import initialize_core


def raw_engine(resource):
    return resource._engine if isinstance(resource, Storage) else resource


def data_store(engine):
    return data_storage(raw_engine(engine))


def research_store(engine):
    return research_storage(raw_engine(engine))


def scheduler(engine):
    engine = raw_engine(engine)
    initialize_core(engine)
    return Tasks(engine)


def domain_tasks(engine, domain):
    from asterion_bindings.task_repository import task_port

    kinds = {
        "data": frozenset({"data.sync", "data.import_csv"}),
        "research": frozenset({"research.backtest"}),
    }
    return task_port(scheduler(engine), kinds[domain])
