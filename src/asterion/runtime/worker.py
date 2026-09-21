"""Workers communicate with serve; they never write the catalog database."""

import logging
import multiprocessing
import re
import time
from concurrent.futures import ProcessPoolExecutor, TimeoutError
from urllib.parse import quote
from uuid import uuid4

import httpx

from asterion.platform.authorization import worker_token
from asterion.platform.config import Settings
from asterion.platform.tasks.execution import ExecutionContext
from asterion.platform.tasks.handlers import PublicationResult
from asterion.runtime.handlers import execution_resources, handlers

log = logging.getLogger(__name__)


def execute_job(settings: Settings, job: dict) -> tuple[bytes, dict]:
    handler = handlers.get(job["kind"])
    with httpx.Client(
        base_url=settings.api_url,
        headers={"Authorization": f"Bearer {worker_token(settings.token)}"},
        timeout=30,
    ) as client:
        active = True

        def post(suffix, body=None, *, lease_in_body=False):
            if not active:
                raise ValueError("Execution transport is closed")
            if not re.fullmatch(r"/[a-z][a-z0-9-]*(?:/[0-9]+)?", suffix):
                raise ValueError("Invalid job-relative operation")
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

        context = ExecutionContext(
            handler.resources,
            execution_resources(settings, handler.kind, post),
        )
        try:
            return handler.execute(context, job["payload"])
        finally:
            active = False
            context.close()


def run_once(settings: Settings, worker_id: str) -> bool:
    with httpx.Client(
        base_url=settings.api_url,
        headers={"Authorization": f"Bearer {worker_token(settings.token)}"},
        timeout=30,
    ) as client:
        response = client.post("/api/v1/jobs/claim", json={"worker_id": worker_id})
        response.raise_for_status()
        job = response.json()
        if job is None:
            return False
        base = f"/api/v1/jobs/{job['id']}"
        try:
            handler = handlers.get(job["kind"])
            with ProcessPoolExecutor(
                max_workers=1, mp_context=multiprocessing.get_context("spawn")
            ) as pool:
                future = pool.submit(execute_job, settings, job)
                while True:
                    try:
                        content, _ = future.result(timeout=max(1, settings.lease_seconds / 3))
                        break
                    except TimeoutError:
                        renewal = client.post(base + "/heartbeat", json={"token": job["token"]})
                        renewal.raise_for_status()
            response = client.post(
                base + handler.publish_suffix,
                content=content,
                headers={"X-Lease-Token": job["token"], "Content-Type": "application/octet-stream"},
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
        except Exception as exc:
            log.exception("Job %s failed", job["id"])
            failure = client.post(base + "/fail", json={"token": job["token"], "error": str(exc)})
            if failure.status_code != 409:
                failure.raise_for_status()
        return True


def run(settings: Settings, once=False):
    settings.require_token()
    worker_id = f"worker-{uuid4()}"
    while True:
        try:
            worked = run_once(settings, worker_id)
        except httpx.HTTPError:
            log.exception("Control plane unavailable")
            worked = False
            if once:
                raise
        if once:
            return
        if not worked:
            time.sleep(2)
