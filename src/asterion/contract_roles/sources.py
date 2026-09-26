"""Source-independent role publication from checksum-verified data versions."""

from datetime import UTC, datetime, timedelta

from asterion_bindings.calendar import TimeVersion
from asterion_bindings.roles import RoleReport, RoleSpec
from pydantic import BaseModel, ConfigDict, Field

from asterion.data.public import ResolutionRequest, VersionAccess, source_contract_catalog


class RoleSourceRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    mapping_version_id: str = Field(min_length=1, max_length=100)
    contracts_version_id: str = Field(min_length=1, max_length=100)
    trading_time: TimeVersion


class RoleSources:
    def __init__(self, versions: VersionAccess):
        self.versions = versions

    def build(self, body: RoleSourceRequest) -> RoleSpec:
        try:
            value = self.versions.read(body.mapping_version_id, limit=10001)
        except KeyError:
            raise ValueError("角色映射来源版本不存在") from None
        version = value["version"]
        manifest = version["manifest"]
        if (
            version["id"] != body.mapping_version_id
            or (
                manifest.get("type", {}).get("id") != "futures.role_mapping"
                or manifest.get("type", {}).get("schema_version") != 1
            )
            or manifest.get("layer") != "STANDARD"
            or manifest.get("state") != "PUBLISHED"
            or manifest.get("demo")
            or not manifest.get("source")
        ):
            raise ValueError("需要已发布的当前标准角色映射快照")
        rows = value["rows"]
        if not 0 < value["total"] <= 10000 or len(rows) != value["total"]:
            raise ValueError("角色映射为空或未完整读取")
        if len({(row["product_id"], row["symbol"]) for row in rows}) != 1:
            raise ValueError("一个角色版本只能绑定单一品种及供应商角色序列")
        catalog = source_contract_catalog(
            self.versions.read,
            body.contracts_version_id,
            sorted({row["target_symbol"] for row in rows}),
        )
        if catalog.inputs[0].source != manifest["source"]:
            raise ValueError("角色映射与合约资料必须同源，不猜测跨源代码")
        # This cutoff resolves identities after acquisition, never certifies historical availability.
        information_at = max(c.provenance.available_at for c in catalog.contracts)
        observed = datetime.fromisoformat(manifest["observed_at"])
        if observed.tzinfo is None:
            raise ValueError("来源观测时刻必须带时区")
        reports = []
        for row in rows:
            resolved = catalog.resolve(
                ResolutionRequest(
                    source=manifest["source"],
                    symbol=row["target_symbol"],
                    trading_day=row["trading_day"],
                    information_at=information_at,
                )
            )
            if resolved.contract.product_id != row["product_id"]:
                raise ValueError("角色目标身份与来源品种不一致")
            reports.append(
                RoleReport(
                    trading_day=row["trading_day"],
                    role=row["role"],
                    contract_id=resolved.contract.id,
                    available_at=row["available_at"],
                    evidence=f"{manifest['source']}: {row['symbol']} → {row['target_symbol']}",
                )
            )
        # Conservative local observation floor for replays; source revisions cannot backdate it.
        created = datetime.fromtimestamp(version["created_at"], UTC)
        if created.timestamp() < version["created_at"]:
            created += timedelta(microseconds=1)
        return RoleSpec(
            schema_version=1,
            origin="provider_report",
            source=manifest["source"],
            source_version=version["id"],
            source_checksum=manifest["checksum"],
            observed_at=max(observed, created),
            product_id=rows[0]["product_id"],
            catalog=catalog,
            trading_time=body.trading_time,
            reports=tuple(sorted(reports, key=lambda r: (r.trading_day, r.role))),
        )

    def verify(self, spec: RoleSpec):
        if len(spec.catalog.inputs) != 1:
            raise ValueError("供应商角色必须绑定唯一固定合约资料输入")
        current = self.build(
            RoleSourceRequest(
                mapping_version_id=spec.source_version,
                contracts_version_id=spec.catalog.inputs[0].version_id,
                trading_time=spec.trading_time,
            )
        )
        if current != spec:
            raise ValueError("角色内容与固定来源证据不一致")
