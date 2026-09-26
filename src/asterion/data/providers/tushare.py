"""Tushare Pro futures source. Planning, transport and field mapping are the
built-in Rust adapter; this class only adapts the internal Python process."""

from contextlib import contextmanager

from asterion_bindings import data_providers

from asterion.data.providers.public import (
    Partition,
    ProviderError,
    ProviderManifest,
    SyncRequest,
)

ID = "tushare"


@contextmanager
def refused():
    try:
        yield
    except ProviderError:
        raise
    except ValueError as error:
        raise ProviderError(str(error)) from None


class Tushare:
    manifest = ProviderManifest.model_validate(
        next(item for item in data_providers.manifests() if item["id"] == ID)
    )

    def probe(self, configuration: dict) -> str:
        with refused():
            return data_providers.probe(ID, configuration)

    def plan(self, request: SyncRequest) -> list[Partition]:
        with refused():
            parts = data_providers.plan(ID, request.model_dump_json())
        return [Partition.model_validate(part) for part in parts]

    def fetch(self, partition: Partition, configuration: dict) -> list[dict]:
        with refused():
            return data_providers.fetch(ID, partition.model_dump_json(), configuration)

    def normalize(self, request: SyncRequest, rows: list[dict]) -> list[dict]:
        with refused():
            return data_providers.normalize(ID, request.model_dump_json(), rows)
