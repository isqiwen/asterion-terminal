"""Research-owned worker contribution entry point."""

from asterion.platform.serialization import canonical
from asterion.platform.tasks.handlers import TaskHandler
from asterion.research.engine import calculate
from asterion.research.service import KIND, validate_input
from asterion.research.strategies import STRATEGY_RESOURCE


def execute(context, payload):
    validate_input(payload)
    return canonical(calculate(payload, context.resource(STRATEGY_RESOURCE))), {}


def task_handlers() -> tuple[TaskHandler, ...]:
    return (TaskHandler(KIND, execute, "/publish-research", resources=(STRATEGY_RESOURCE,)),)
