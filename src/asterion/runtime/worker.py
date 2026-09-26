"""Workers communicate with serve; they never write the catalog database."""

import signal
import sys
import threading
import time
from urllib.parse import quote
from uuid import uuid4

import httpx
from asterion_bindings.authority import worker_token
from asterion_bindings.diagnostics import record_event
from asterion_bindings.task_handlers import PublicationResult
from asterion_bindings.task_process import TaskProcess
from asterion_bindings.tasks import ExecutionContext

from asterion.platform.communication.http import outbound
from asterion.platform.config import Settings
from asterion.runtime.handlers import execution_resources, handlers


def computation(settings: Settings, job: dict):
    return TaskProcess(
        [sys.executable, "-m", "asterion.runtime.task_child"],
        job["communication"],
        {
            "settings": {
                "data_root": str(settings.data_root),
                "api_url": settings.api_url,
                "token": settings.token,
                "lease_seconds": settings.lease_seconds,
            },
            "job": job,
        },
        limits={
            "input_bytes": 256 * 1024 * 1024,
            "artifact_bytes": 1024 * 1024 * 1024,
            "metadata_bytes": 1024 * 1024,
            "stderr_bytes": 1024 * 1024,
        },
    )


def execute_job(settings: Settings, job: dict) -> tuple[bytes, dict]:
    from asterion_bindings.communication import activate

    with activate(job["communication"]):
        return _execute_job(settings, job)


def _execute_job(settings: Settings, job: dict) -> tuple[bytes, dict]:
    handler = handlers.get(job["kind"])
    with httpx.Client(
        event_hooks={"request": [outbound]},
        base_url=settings.api_url,
        headers={"Authorization": f"Bearer {worker_token(settings.token)}"},
        timeout=30,
    ) as client:
        requests = handler.request_scope()

        def post(suffix, body=None, *, lease_in_body=False):
            requests.authorize(suffix)
            payload = body
            if lease_in_body:
                payload = {**(body or {}), "token": job["token"]}
            try:
                response = client.post(
                    f"/api/v1/jobs/{quote(job['id'], safe='')}{suffix}",
                    json=payload,
                    headers={"X-Lease-Token": job["token"]},
                )
                response.raise_for_status()
            except httpx.HTTPError:
                raise ValueError("任务执行通道请求失败，请检查服务与租约状态") from None
            try:
                return response.json()
            except ValueError:
                raise ValueError("任务执行通道返回无效响应") from None

        try:
            with execution_resources(settings, handler.kind, post) as resources:
                context = ExecutionContext(handler.resources, resources)
                try:
                    return handler.execute(context, job["payload"])
                finally:
                    context.close()
        finally:
            requests.close()


def run_once(settings: Settings, worker_id: str) -> bool:
    with httpx.Client(
        event_hooks={"request": [outbound]},
        base_url=settings.api_url,
        headers={"Authorization": f"Bearer {worker_token(settings.token)}"},
        timeout=30,
    ) as client:
        response = client.post("/api/v1/jobs/claim", json={"worker_id": worker_id})
        response.raise_for_status()
        job = response.json()
        if job is None:
            return False
        client.event_hooks["request"] = [lambda request: outbound(request, job["communication"])]
        base = f"/api/v1/jobs/{job['id']}"
        try:
            handler = handlers.get(job["kind"])
            with computation(settings, job) as process:
                while not process.poll(min(60, max(1, settings.lease_seconds / 3))):
                    renewal = client.post(base + "/heartbeat", json={"token": job["token"]})
                    renewal.raise_for_status()
                with process.take_result() as artifact:
                    response = client.post(
                        base + handler.publish_suffix,
                        content=artifact,
                        headers={
                            "X-Lease-Token": job["token"],
                            "Content-Type": "application/octet-stream",
                        },
                    )
            try:
                result = response.json()
            except ValueError:
                result = None
            handler.check_publication(
                PublicationResult(
                    response.status_code,
                    result.get("detail") if isinstance(result, dict) else None,
                )
            )
            response.raise_for_status()
        except Exception as exc:  # noqa: BLE001 - report the task failure without logging its payload
            record_event(
                settings.data_root / ".diagnostics",
                "worker",
                "execution_failed",
                context=job.get("communication"),
            )
            failure = client.post(base + "/fail", json={"token": job["token"], "error": str(exc)})
            if failure.status_code != 409:
                failure.raise_for_status()
        return True


class _WorkerStopped(BaseException):
    pass


def run(settings: Settings, once=False):
    settings.require_token()
    worker_id = f"worker-{uuid4()}"
    previous_signals = {}

    def stop(_number, _frame):
        raise _WorkerStopped

    if threading.current_thread() is threading.main_thread():
        previous_signals = {
            number: signal.signal(number, stop) for number in (signal.SIGTERM, signal.SIGINT)
        }
    try:
        while True:
            try:
                worked = run_once(settings, worker_id)
            except httpx.HTTPError:
                record_event(
                    settings.data_root / ".diagnostics", "worker", "control_plane_unavailable"
                )
                worked = False
                if once:
                    raise
            if once:
                return
            if not worked:
                time.sleep(2)
    except _WorkerStopped:
        return
    finally:
        for number, handler in previous_signals.items():
            signal.signal(number, handler)
