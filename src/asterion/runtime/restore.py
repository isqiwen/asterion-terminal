"""Product restore assembly in one physical database transaction."""

from asterion_bindings.recovery import RestoreScope, restore_steps
from asterion_bindings.task_repository import Tasks


def isolate_restore(engine, plugins, bind_inputs):
    """Native required-operation checks precede the shared tasks/domain commit."""
    tasks = Tasks(engine)
    scope = RestoreScope()
    try:
        with engine.begin() as conn:
            inputs = bind_inputs(conn, plugins, scope)
            steps = restore_steps(plugins, inputs)
            cancelled = tasks.isolate(conn, "恢复后隔离：请检查原任务后显式重试")
            for identifier, step in steps:
                step.apply(inputs[identifier])
            scope.verify()
            return cancelled
    finally:
        scope.close()
