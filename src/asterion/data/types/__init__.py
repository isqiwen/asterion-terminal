"""Built-in data types: fixed Rust data-store definitions; no dynamic registration."""

from asterion_bindings import data_catalog, data_types

from asterion.data.providers.public import ProviderError
from asterion.data.types.public import DataType, TypeManifest, TypeRegistry


def daily_chart(rows, available_at):
    """The Rust data store's daily chart table and its manifest."""
    return data_catalog.daily_chart(rows, available_at)


def _builtin(manifest: dict) -> DataType:
    type_id = manifest["id"]

    def validate(rows):
        try:
            data_types.validate(type_id, rows)
        except ValueError as error:
            raise ProviderError(str(error)) from None

    def coverage(rows, start, end):
        try:
            return data_types.coverage(type_id, len(rows), start, end)
        except ValueError as error:
            raise ProviderError(str(error)) from None

    chart = daily_chart if type_id == "futures.daily" else None
    return DataType(TypeManifest.model_validate(manifest), validate, coverage, chart)


def builtin_types():
    return TypeRegistry(tuple(_builtin(manifest) for manifest in data_types.manifests()))
