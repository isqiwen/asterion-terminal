"""Task identity checks shared by admission, collection and publication; the
Rust data service owns them."""

from asterion_bindings import data_sync
from asterion_bindings.catalog import SourceIdentity

from asterion.data.sources import refused


def task_identity(payload: dict) -> SourceIdentity | None:
    """Check a task's request, type and fixed identity; its identity when contract-bound."""
    with refused():
        identity = data_sync.task_identity(payload)
    return None if identity is None else SourceIdentity.model_validate(identity)
