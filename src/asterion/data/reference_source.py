"""Identity releases from fixed, verified standard contract observations; the
catalogs are built by the Rust data store from the version the reader returns."""

from asterion_bindings import data_identity
from asterion_bindings.catalog import CatalogInput, ImportIdentity, ReferenceCatalog
from pydantic import BaseModel, ConfigDict, Field


class SourceCatalogRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version_id: str = Field(min_length=1, max_length=100)
    symbols: list[str] = Field(min_length=1, max_length=10000)


def validate_import_inputs(identity: ImportIdentity, reader):
    """Verify external evidence in the application workflow, outside domain bindings."""
    for item in identity.catalog.inputs:
        try:
            result = reader(item.version_id, limit=1)
        except KeyError:
            raise ValueError("导入身份目录的固定资料版本不存在") from None
        validate_catalog_input(item, result["version"]["manifest"])


def validate_catalog_input(item: CatalogInput, manifest: dict):
    data_identity.check_input(item.model_dump(mode="json"), manifest)


def source_catalog(reader, request: SourceCatalogRequest) -> ReferenceCatalog:
    """Selection never chooses a newer version or infers identity from a market code."""
    try:
        result = reader(request.version_id, limit=10001)
    except KeyError:
        raise ValueError("合约资料版本不存在，请先同步合约资料") from None
    return ReferenceCatalog.model_validate(
        data_identity.source_catalog(result, request.version_id, request.symbols)
    )


def product_catalog(reader, version_id: str, product_id: str) -> ReferenceCatalog:
    """All product identities in one fixed source; no claim of exchange-wide completeness."""
    try:
        value = reader(version_id, limit=10001)
    except KeyError:
        raise ValueError("固定合约资料版本不存在") from None
    return ReferenceCatalog.model_validate(
        data_identity.product_catalog(value, version_id, product_id)
    )
