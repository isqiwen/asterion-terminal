"""Built-in fixed-input research, routes and data reference reporting."""

from asterion.contract_rules.public import RULE_ACCESS
from asterion.data.public import VERSION_ACCESS
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.plugins import Activation, Context, Plugin
from asterion.platform.resources import STORAGE, TASKS
from asterion.research.backup import check
from asterion.research.public import version_references
from asterion.research.routes import router
from asterion.research.service import Backtests
from asterion.research.strategies import STRATEGIES
from asterion.research.worker import task_handlers


def activate(context: Context):
    access = context.require(ACCOUNT_ACCESS)
    versions = context.require(VERSION_ACCESS)
    service = Backtests(
        context.resource(STORAGE),
        context.resource(TASKS),
        versions,
        context.require(RULE_ACCESS),
        context.require(STRATEGIES),
    )

    def references(transaction, version_id):
        with context.resource(STORAGE).borrow(transaction) as reader:
            return version_references(reader, version_id)

    return Activation(
        close=context.resource(STORAGE).close,
        routers=(router(service, access.account, access.owner),),
        hooks={"data.references": (references,)},
    )


plugin = Plugin(
    "asterion.research",
    ("asterion.identity", "asterion.data", "asterion.contract_rules", "asterion.strategy_catalog"),
    activate,
    handlers=task_handlers(),
    consumes=(ACCOUNT_ACCESS, VERSION_ACCESS, RULE_ACCESS, STRATEGIES),
    resources=(STORAGE, TASKS),
    backup=check,
)
