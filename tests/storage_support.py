"""Test-owned assembly: retain raw engines for corruption fixtures and scheduler assertions."""

from asterion.distribution_storage import data_storage, identity_storage, research_storage
from asterion.platform.storage import Storage
from asterion.platform.tasks.service import Tasks


def raw_engine(resource):
    return resource._engine if isinstance(resource, Storage) else resource


def data_store(engine):
    return data_storage(raw_engine(engine))


def identity_store(engine):
    return identity_storage(raw_engine(engine))


def research_store(engine):
    return research_storage(raw_engine(engine))


def scheduler(engine):
    return Tasks(raw_engine(engine))


def domain_tasks(engine, domain):
    from asterion.platform.task_port import task_port

    kinds = {
        "data": frozenset({"data.sync", "data.import_csv"}),
        "research": frozenset({"research.backtest"}),
    }
    return task_port(scheduler(engine), kinds[domain])
