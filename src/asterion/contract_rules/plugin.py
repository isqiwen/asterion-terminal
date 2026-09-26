"""Rules management routes and the public research dependency."""

from asterion_bindings.plugin_host import Activation, Plugin
from asterion_bindings.rules import RulePeriod, RuleSpec, RuleVersion, SettlementEvidence
from fastapi import APIRouter, Depends

from asterion.contract_rules.backup import check
from asterion.contract_rules.mapping import MappingPreview, MappingRequest
from asterion.contract_rules.public import RULE_ACCESS, RuleAccess
from asterion.contract_rules.service import Rules
from asterion.contract_rules.settlement import SettlementConfirmation, SettlementRequest
from asterion.data.public import VERSION_ACCESS
from asterion.identity.public import ACCOUNT_ACCESS
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

    @router.post("/settlement/confirm", response_model=RulePeriod)
    def settlement_confirm(body: SettlementConfirmation):
        return service.settlement.confirm(body)

    return Activation(
        exports={RULE_ACCESS: RuleAccess(service.read)},
        routers=(router,),
        close=storage.close,
    )


plugin = Plugin(
    "asterion.contract_rules",
    ("asterion.identity", "asterion.data"),
    activate,
    provides=(RULE_ACCESS,),
    consumes=(ACCOUNT_ACCESS, VERSION_ACCESS),
    resources=(STORAGE,),
    backup=check,
)
