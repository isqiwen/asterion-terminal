"""Build identity releases from fixed, verified standard contract observations."""

from datetime import UTC, datetime, timedelta

from pydantic import BaseModel, ConfigDict, Field

from asterion.data.reference import (
    CatalogInput,
    Contract,
    Product,
    Provenance,
    ReferenceCatalog,
    SourceSymbol,
)
from asterion.data.types import Contract as ContractRow
from asterion.data.types import builtin_types


class SourceCatalogRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version_id: str = Field(min_length=1, max_length=100)
    symbols: list[str] = Field(min_length=1, max_length=10000)


def validate_catalog_input(item: CatalogInput, manifest: dict):
    if (
        manifest.get("source") != item.source
        or manifest.get("checksum") != item.checksum
        or manifest.get("layer") != "STANDARD"
        or manifest.get("state") != "PUBLISHED"
        or manifest.get("demo")
        or manifest.get("type", {}).get("id") != "futures.contracts"
        or manifest.get("type", {}).get("schema_version")
        != builtin_types().get("futures.contracts").manifest.schema_version
    ):
        raise ValueError("目录输入不是匹配的当前标准合约资料版本")


def source_catalog(reader, request: SourceCatalogRequest) -> ReferenceCatalog:
    """Selection never chooses a newer version or infers identity from a market code."""
    if len(set(request.symbols)) != len(request.symbols):
        raise ValueError("选择的来源代码重复")
    try:
        result = reader(request.version_id, limit=10001)
    except KeyError:
        raise ValueError("合约资料版本不存在，请先同步合约资料") from None
    version = result["version"]
    manifest = version["manifest"]
    if version["id"] != request.version_id:
        raise ValueError("合约资料版本与请求不一致")
    evidence = CatalogInput(
        version_id=version["id"], checksum=manifest["checksum"], source=manifest["source"]
    )
    validate_catalog_input(evidence, manifest)
    if not 0 < result["total"] <= 10000 or result["total"] != len(result["rows"]):
        raise ValueError("合约资料未完整读取，不能发布身份目录")
    selected = set(request.symbols)
    rows = [ContractRow.model_validate(row) for row in result["rows"] if row["symbol"] in selected]
    if {row.symbol for row in rows} != selected:
        raise ValueError("所选来源代码不在固定合约资料版本中")
    # Acquisition is not proof of historical publication. Do not backdate availability.
    created = datetime.fromtimestamp(version["created_at"], UTC)
    if created.timestamp() < version["created_at"]:
        created += timedelta(microseconds=1)
    provenance = Provenance(
        source=evidence.source,
        source_version=evidence.version_id,
        observed_at=manifest["observed_at"],
        available_at=manifest["available_at"],
    )
    provenance = provenance.model_copy(
        update={"available_at": max(provenance.available_at, created)}
    )
    products, contracts, symbols = {}, [], []
    for row in sorted(rows, key=lambda row: (row.exchange, row.symbol, row.listed)):
        if row.delivery_month is None or row.delisted is None:
            raise ValueError(f"{row.symbol} 缺少完整交割年月或最后交易日，不能发布身份目录")
        product_id = f"{row.exchange}.{row.product}"
        product = Product.model_validate(
            {
                "id": product_id,
                "exchange": row.exchange,
                "name": row.product,  # Source product code is a label, not an inferred Chinese name.
                "currency": row.currency,
                "provenance": provenance,
            }
        )
        if product_id in products and products[product_id] != product:
            raise ValueError("同一品种的资料存在冲突")
        products[product_id] = product
        contract = Contract(
            id=f"{product_id}.{row.delivery_month.replace('-', '')}.{row.listed:%Y%m%d}",
            product_id=product_id,
            delivery_month=row.delivery_month,
            listed_on=row.listed,
            last_trade_on=row.delisted,
            last_delivery_on=row.last_delivery_on,
            provenance=provenance,
        )
        contracts.append(contract)
        symbols.append(
            SourceSymbol(
                source=evidence.source,
                symbol=row.symbol,
                contract_id=contract.id,
                valid_from=row.listed,
                valid_until=row.delisted,
                provenance=provenance,
            )
        )
    return ReferenceCatalog(
        schema_version=2,
        inputs=(evidence,),
        products=tuple(products[key] for key in sorted(products)),
        contracts=tuple(contracts),
        symbols=tuple(symbols),
    )


def product_catalog(reader, version_id: str, product_id: str) -> ReferenceCatalog:
    """All product identities in one fixed source; no claim of exchange-wide completeness."""
    try:
        value = reader(version_id, limit=10001)
    except KeyError:
        raise ValueError("固定合约资料版本不存在") from None
    if not 0 < value["total"] <= 10000 or len(value["rows"]) != value["total"]:
        raise ValueError("品种候选资料未完整读取")
    rows = [ContractRow.model_validate(row) for row in value["rows"]]
    symbols = sorted({row.symbol for row in rows if f"{row.exchange}.{row.product}" == product_id})
    if not symbols:
        raise ValueError("固定合约资料中没有该品种")
    return source_catalog(
        lambda identifier, *, limit: value,
        SourceCatalogRequest(version_id=version_id, symbols=symbols),
    )
