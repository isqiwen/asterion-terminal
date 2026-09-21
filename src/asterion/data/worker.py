"""Data-owned worker contribution entry point."""

from asterion.data.importing import encode_import
from asterion.data.public import CREDENTIALS, SYNC_REPORTER
from asterion.platform.authorization import Grant
from asterion.platform.resources import DATA_ROOT
from asterion.platform.tasks.execution import ExecutionContext
from asterion.platform.tasks.handlers import PublicationResult, TaskHandler


def import_csv(context: ExecutionContext, payload: dict) -> tuple[bytes, dict]:
    return encode_import(payload)


def sync_data(context: ExecutionContext, payload: dict) -> tuple[bytes, dict]:
    from asterion.data.sync import collect

    reporter = context.resource(SYNC_REPORTER)
    return collect(
        payload,
        context.resource(DATA_ROOT),
        context.resource(CREDENTIALS),
        reporter.progress,
        reporter.checkpoint,
        reporter.resume(),
    ), {}


def check_data_publication(response: PublicationResult) -> None:
    if response.status_code == 422:
        detail = response.detail
        raise ValueError(detail if isinstance(detail, str) else "数据发布校验失败")
    response.raise_for_status()


def task_handlers() -> tuple[TaskHandler, ...]:
    return (
        TaskHandler("data.import_csv", import_csv, "/publish"),
        TaskHandler(
            "data.sync",
            sync_data,
            "/publish-data",
            check_data_publication,
            resources=(DATA_ROOT, CREDENTIALS, SYNC_REPORTER),
            requests=(
                Grant("/jobs/:id/progress", ("POST",)),
                Grant("/jobs/:id/resume", ("POST",)),
                Grant("/jobs/:id/observations/:id", ("POST",)),
            ),
        ),
    )
