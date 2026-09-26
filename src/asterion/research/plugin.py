"""Built-in fixed-input research, routes and data reference reporting."""

from asterion_bindings.plugin_host import Activation, Context, Plugin

from asterion.contract_rules.public import RULE_ACCESS
from asterion.data.public import VERSION_ACCESS
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.resources import STORAGE, TASKS
from asterion.research.backup import check
from asterion.research.execution import EXECUTION
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
        context.resource(EXECUTION),
    )

    return Activation(
        close=context.resource(STORAGE).close,
        routers=(router(service, access.account, access.owner),),
    )


plugin = Plugin(
    "asterion.research",
    ("asterion.identity", "asterion.data", "asterion.contract_rules", "asterion.strategy_catalog"),
    activate,
    handlers=task_handlers(),
    consumes=(ACCOUNT_ACCESS, VERSION_ACCESS, RULE_ACCESS, STRATEGIES),
    resources=(STORAGE, TASKS, EXECUTION),
    backup=check,
)
