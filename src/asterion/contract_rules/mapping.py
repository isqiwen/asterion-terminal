"""Read standardized contract snapshots into incomplete research rule drafts."""

from decimal import Decimal, InvalidOperation

from pydantic import BaseModel, ConfigDict, Field, model_validator

from asterion.contract_rules.public import ContractBasis, RuleSpec
from asterion.data.public import Contract, VersionAccess, source_contract


class MappingRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version_id: str = Field(min_length=1, max_length=100)
    contract_id: str = Field(
        pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+\.[0-9]{6}\.[0-9]{8}$"
    )


class MappingPreview(BaseModel):
    basis: ContractBasis
    contract: Contract
    suggested_multiplier: str | None
    multiplier_note: str = Field(min_length=1)
    missing: list[str]

    @model_validator(mode="after")
    def valid_multiplier(self):
        if self.suggested_multiplier is not None:
            try:
                value = Decimal(self.suggested_multiplier)
            except InvalidOperation:
                raise ValueError("标准合约资料的建议乘数无效") from None
            if not value.is_finite() or not 0 < value <= 10**6:
                raise ValueError("标准合约资料的建议乘数无效")
        return self


class SourceMapping:
    def __init__(self, versions: VersionAccess):
        self.versions = versions

    def preview(self, body: MappingRequest):
        try:
            source = self.versions.read(body.version_id, limit=10001)
        except KeyError:
            raise ValueError("合约资料版本不存在，请先同步合约资料") from None
        version = source["version"]
        manifest = version["manifest"]
        if (
            version["id"] != body.version_id
            or not manifest["source"]
            or manifest["type"]["id"] != "futures.contracts"
            or manifest["type"].get("schema_version") != 4
            or manifest["layer"] != "STANDARD"
            or manifest.get("demo")
        ):
            raise ValueError("请选择当前格式的标准合约资料版本；需要时重新同步")
        if source["total"] > 10000 or source["total"] != len(source["rows"]):
            raise ValueError("合约资料未完整读取，不能生成规则依据")
        actual, row = source_contract(source, body.contract_id)
        fields = {
            "contract",
            "suggested_multiplier",
            "multiplier_note",
            "symbol",
            "exchange",
            "name",
            "listed",
            "delisted",
            "trade_unit",
            "per_unit",
            "multiplier",
            "quote_unit",
            "quote_unit_desc",
        }
        if not fields.issubset(row) or "connection_id" not in manifest.get("origin", {}):
            raise ValueError("合约资料字段或来源记录不完整，请重新同步")
        basis = ContractBasis(
            provider=manifest["source"],
            contract_id=actual.id,
            version_id=version["id"],
            checksum=manifest["checksum"],
            connection_id=manifest["origin"]["connection_id"],
            **{
                key: row[key]
                for key in (
                    "symbol",
                    "exchange",
                    "name",
                    "listed",
                    "delisted",
                    "trade_unit",
                    "per_unit",
                    "multiplier",
                    "quote_unit",
                    "quote_unit_desc",
                )
            },
        )
        multiplier = row["suggested_multiplier"]
        note = row["multiplier_note"]
        return MappingPreview(
            basis=basis,
            contract=actual,
            suggested_multiplier=multiplier,
            multiplier_note=note,
            missing=[
                *([] if multiplier else ["合约乘数"]),
                "最小变动价位",
                "生效期间",
                "手续费",
                "保证金比例",
            ],
        )

    def validate(self, spec: RuleSpec):
        if spec.basis is None:
            return
        current = self.preview(
            MappingRequest(version_id=spec.basis.version_id, contract_id=spec.contract.id)
        )
        if current.basis != spec.basis or current.contract != spec.contract:
            raise ValueError("合约规则的来源快照与固定资料版本不一致")
