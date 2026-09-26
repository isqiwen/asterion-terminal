"""Data source connections and configurations for the internal Python process.

Thin calls into the Rust data service, which also serves the HTTP operations
from the entry; nothing here re-implements validation, revisions or snapshots.
"""

from contextlib import contextmanager

from asterion_bindings import data_sources
from asterion_bindings.data_sources import SourceCredentials
from asterion_bindings.task_repository import Conflict

from asterion.data.providers.public import ProviderError


@contextmanager
def refused():
    try:
        yield
    except (Conflict, ProviderError):
        raise
    except ValueError as error:
        raise ProviderError(str(error)) from None


class DataSources:
    def __init__(self, engine, registry, credentials: SourceCredentials):
        self.engine, self.registry, self.credentials = engine, registry, credentials

    def manifests(self) -> list[dict]:
        return [plugin.manifest.model_dump(mode="json") for plugin in self.registry.all()]

    def listing(self) -> list[dict]:
        with self.engine.connect() as conn, refused():
            return self.credentials.listing(conn, self.manifests())

    def state(self, owner: str) -> dict:
        with self.engine.connect() as conn, refused():
            return self.credentials.state(conn, self.manifests(), owner)

    def apply(self, owner: str, expected_revision: int, values=None, secrets=None) -> dict:
        update = {
            "expected_revision": expected_revision,
            "values": values or {},
            "secrets": secrets or {},
        }
        with self.engine.begin() as conn, refused():
            return self.credentials.apply(conn, self.manifests(), owner, update)

    def connection(self, identifier: str) -> dict:
        with self.engine.connect() as conn, refused():
            return data_sources.connection_state(conn, self.manifests(), identifier)

    def create(self, provider: str, name: str) -> dict:
        with self.engine.begin() as conn, refused():
            return data_sources.connection_create(conn, self.manifests(), provider, name)

    def update(self, identifier: str, expected_revision: int, name: str, state: str) -> dict:
        body = {"expected_revision": expected_revision, "name": name, "state": state}
        with self.engine.begin() as conn, refused():
            return data_sources.connection_update(conn, self.manifests(), identifier, body)

    def fix_for_task(self, conn, owner: str, provider: str) -> dict:
        with refused():
            return self.credentials.fix_for_task(conn, self.manifests(), owner, provider)
