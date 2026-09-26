"""The complete current core storage contract, used by runtime and acceptance tools."""

from asterion_bindings.storage import initialize_schema

from asterion.platform.communication.events import TABLES
from asterion.platform.store import jobs

CORE_TABLES = (jobs, *TABLES)


def initialize_core(engine):
    initialize_schema(engine, CORE_TABLES)
