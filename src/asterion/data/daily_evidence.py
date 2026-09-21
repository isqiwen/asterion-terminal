"""Fixed daily observations exposed by the data owner, with local availability only."""

from datetime import UTC, datetime, timedelta

from pydantic import AwareDatetime, BaseModel, ConfigDict, TypeAdapter

from asterion.data.reference import SourceIdentity
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.types import DailyBar, builtin_types


class DailyObservation(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    bar: DailyBar
    available_at: AwareDatetime


class DailyEvidence(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    version_id: str
    checksum: str
    source: str
    identity: SourceIdentity
    observations: tuple[DailyObservation, ...]


def fixed_daily_evidence(reader, identifier: str) -> DailyEvidence:
    try:
        value = reader(identifier, limit=100001)
    except KeyError:
        raise ValueError("固定日线版本不存在") from None
    version = value["version"]
    manifest = version["manifest"]
    definition = builtin_types().get("futures.daily").manifest
    if (
        version["id"] != identifier
        or manifest.get("type", {}).get("id") != definition.id
        or manifest.get("type", {}).get("schema_version") != definition.schema_version
        or manifest.get("layer") != "STANDARD"
        or manifest.get("state") != "PUBLISHED"
        or manifest.get("demo")
        or manifest.get("format") not in {"parquet", "partition_manifest"}
    ):
        raise ValueError("需要当前已发布的标准日线来源版本")
    fields = manifest["type"].get("fields", [])
    units = {field["name"]: field.get("unit") for field in fields}
    if len(units) != len(fields) or any(units.get(k) != "手" for k in ("vol", "oi")):
        raise ValueError("日线成交量和持仓量必须明确标准手数单位")
    if not 0 < value["total"] <= 100000 or len(value["rows"]) != value["total"]:
        raise ValueError("固定日线为空或未完整读取")
    identity = SourceIdentity.model_validate(manifest.get("contract_identity"))
    if identity.source != manifest.get("source"):
        raise ValueError("日线来源与固定合约身份不一致")
    restored = source_catalog(
        reader,
        SourceCatalogRequest(
            version_id=identity.catalog.inputs[0].version_id,
            symbols=sorted({s.symbol for s in identity.catalog.symbols}),
        ),
    )
    if restored != identity.catalog:
        raise ValueError("日线身份与固定合约资料证据不一致")
    rows = [DailyBar.model_validate(row) for row in value["rows"]]
    normalized = [row.model_dump(mode="json") for row in rows]
    identity.validate_rows(normalized, rows[0].exchange)
    keys = [(r.contract, r.trading_day) for r in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("固定日线存在重复记录")
    aware = TypeAdapter(AwareDatetime)
    observed = aware.validate_python(manifest["observed_at"])
    available = aware.validate_python(manifest["available_at"])
    created = datetime.fromtimestamp(version["created_at"], UTC)
    if created.timestamp() < version["created_at"]:
        created += timedelta(microseconds=1)
    floor = max(observed, available, created, identity.information_at)
    provenance = value.get("row_sources")
    if manifest["format"] == "partition_manifest" and (
        provenance is None or len(provenance) != len(rows)
    ):
        raise ValueError("累积日线缺少逐行采集证据")
    observations = []
    for index, bar in enumerate(rows):
        local = floor
        if provenance is not None:
            local = max(local, aware.validate_python(provenance[index]["observed_at"]))
        observations.append(DailyObservation(bar=bar, available_at=local))
    return DailyEvidence(
        version_id=identifier,
        checksum=manifest["checksum"],
        source=identity.source,
        identity=identity,
        observations=tuple(observations),
    )
