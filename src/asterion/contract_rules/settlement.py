"""Read checksum-verified settlement snapshots through the declared data capability."""

from datetime import date

from asterion_bindings.rules import (
    RulePeriod,
    RuleSpec,
    SettlementBasis,
    SettlementEvidence,
    SettlementRow,
)

from asterion.contract_rules.mapping import MappingRequest
from asterion.data.public import SourceIdentity, VersionAccess


class SettlementRequest(MappingRequest):
    trading_day: date


class SettlementConfirmation(SettlementBasis):
    start: date
    end: date


class SettlementMapping:
    def __init__(self, versions: VersionAccess):
        self.versions = versions

    def preview(self, body: SettlementRequest) -> SettlementEvidence:
        try:
            source = self.versions.read(body.version_id, limit=10001)
        except KeyError:
            raise ValueError("结算参数版本不存在，请先同步每日结算参数") from None
        version = source["version"]
        manifest = version["manifest"]
        if (
            version["id"] != body.version_id
            or not manifest["source"]
            or manifest["type"]["id"] != "futures.settlement"
            or manifest["type"].get("schema_version") != 1
            or manifest["layer"] != "STANDARD"
            or manifest.get("demo")
        ):
            raise ValueError("请选择标准每日结算参数版本")
        if source["total"] > 10000 or source["total"] != len(source["rows"]):
            raise ValueError("结算参数未完整读取")
        identity = SourceIdentity.model_validate(manifest.get("contract_identity"))
        if identity.source != manifest["source"]:
            raise ValueError("结算来源与固定合约身份来源不一致")
        identity.validate_rows(source["rows"], manifest["scope"]["exchange"])
        actual = identity.resolve(body.trading_day)
        if actual.id != body.contract_id:
            raise ValueError("结算版本与所选规范合约身份不一致")
        rows = [r for r in source["rows"] if r.get("trading_day") == str(body.trading_day)]
        if len(rows) != 1:
            raise ValueError("没有唯一匹配的合约和参数交易日")
        if "connection_id" not in manifest.get("origin", {}) or "observed_at" not in manifest:
            raise ValueError("结算参数来源记录不完整")
        row = SettlementRow.model_validate(rows[0])
        return SettlementEvidence(
            provider=manifest["source"],
            version_id=version["id"],
            checksum=manifest["checksum"],
            connection_id=manifest["origin"]["connection_id"],
            observed_at=manifest["observed_at"],
            row=row,
            contract=actual,
        )

    def verify(self, basis: SettlementBasis):
        evidence = basis.evidence
        current = self.preview(
            SettlementRequest(
                version_id=evidence.version_id,
                contract_id=evidence.contract.id,
                trading_day=evidence.row.trading_day,
            )
        )
        if current != evidence:
            raise ValueError("结算依据与固定数据版本不一致")

    def confirm(self, body: SettlementConfirmation) -> RulePeriod:
        basis = SettlementBasis.model_validate(body.model_dump(exclude={"start", "end"}))
        self.verify(basis)
        return basis.period(body.start, body.end)

    def validate(self, spec: RuleSpec):
        for period in spec.periods:
            if period.settlement_basis:
                self.verify(period.settlement_basis)
