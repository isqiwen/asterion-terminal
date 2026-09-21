"""Rules management routes and the public research dependency."""

from fastapi import APIRouter, Depends

from asterion.contract_rules.backup import check
from asterion.contract_rules.mapping import MappingPreview, MappingRequest
from asterion.contract_rules.public import RULE_ACCESS, Period, RuleAccess, RuleSpec, RuleVersion
from asterion.contract_rules.service import Rules
from asterion.contract_rules.settlement import SettlementConfirmation, SettlementRequest
from asterion.contract_rules.settlement_contract import SettlementEvidence
from asterion.data.public import VERSION_ACCESS
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.plugins import Activation, Plugin
from asterion.platform.resources import STORAGE


def activate(context):
    storage = context.resource(STORAGE)
    service = Rules(storage, context.require(VERSION_ACCESS))
    access = context.require(ACCOUNT_ACCESS)
    router = APIRouter(prefix="/api/v1/contract-rules", dependencies=[Depends(access.account)])

    @router.get("", response_model=list[RuleVersion])
    def listing():
        return service.list()

    @router.post("", response_model=RuleVersion)
    def save(body: RuleSpec):
        return service.save(body)

    @router.post("/source/preview", response_model=MappingPreview)
    def preview(body: MappingRequest):
        return service.mapping.preview(body)

    @router.post("/settlement/preview", response_model=SettlementEvidence)
    def settlement_preview(body: SettlementRequest):
        return service.settlement.preview(body)

    @router.post("/settlement/confirm", response_model=Period)
    def settlement_confirm(body: SettlementConfirmation):
        return service.settlement.confirm(body)

    def references(transaction, version_id):
        from sqlalchemy import select

        from asterion.contract_rules.service import rules

        with storage.borrow(transaction) as reader:
            count = sum(
                spec["contract"]["provenance"]["source_version"] == version_id
                or (spec["basis"] or {}).get("version_id") == version_id
                or any(
                    p["settlement_basis"]
                    and p["settlement_basis"]["evidence"]["version_id"] == version_id
                    for p in spec["periods"]
                )
                for spec in reader.execute(select(rules.c.spec)).scalars()
            )
        return {"contract_rules": count}

    return Activation(
        exports={RULE_ACCESS: RuleAccess(service.read)},
        routers=(router,),
        close=storage.close,
        hooks={"data.references": (references,)},
    )


plugin = Plugin(
    "asterion.contract_rules",
    ("asterion.identity", "asterion.data", "asterion.trading_time"),
    activate,
    provides=(RULE_ACCESS,),
    consumes=(ACCOUNT_ACCESS, VERSION_ACCESS),
    resources=(STORAGE,),
    backup=check,
)
