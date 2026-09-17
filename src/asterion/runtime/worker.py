"""Workers communicate with serve; they never write the catalog database."""

import logging
import multiprocessing
import time
from concurrent.futures import ProcessPoolExecutor, TimeoutError
from uuid import uuid4

import httpx

from asterion.data.public import encode_csv
from asterion.platform.config import Settings

log = logging.getLogger(__name__)


def execute_job(settings: Settings, job: dict):
    if job["kind"] == "data.import_csv":
        return encode_csv(job["payload"]["csv"])
    if job["kind"] == "data.sync":
        from asterion.data.sync import collect

        with httpx.Client(
            base_url=settings.api_url,
            headers={"Authorization": f"Bearer {settings.token}"},
            timeout=30,
        ) as client:

            def progress(completed, total):
                response = client.post(
                    f"/api/v1/jobs/{job['id']}/progress",
                    json={"token": job["token"], "completed": completed, "total": total},
                )
                response.raise_for_status()

            return collect(job["payload"], settings.data_root, settings.token, progress), {}
    raise ValueError("Unsupported job kind")


def run_once(settings: Settings, worker_id: str) -> bool:
    with httpx.Client(
        base_url=settings.api_url, headers={"Authorization": f"Bearer {settings.token}"}, timeout=30
    ) as client:
        response = client.post("/api/v1/jobs/claim", json={"worker_id": worker_id})
        response.raise_for_status()
        job = response.json()
        if job is None:
            return False
        base = f"/api/v1/jobs/{job['id']}"
        try:
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
                base + ("/publish-data" if job["kind"] == "data.sync" else "/publish"),
                content=content,
                headers={"X-Lease-Token": job["token"], "Content-Type": "application/octet-stream"},
            )
            if response.status_code == 422 and job["kind"] == "data.sync":
                detail = response.json().get("detail")
                raise ValueError(detail if isinstance(detail, str) else "数据发布校验失败")
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
