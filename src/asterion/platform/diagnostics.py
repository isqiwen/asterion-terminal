"""Observed local service state, without inferring external provider connectivity."""

import json
import time
from pathlib import Path
from typing import Literal

from pydantic import BaseModel
from sqlalchemy import text
from sqlalchemy.exc import SQLAlchemyError


class ServiceState(BaseModel):
    id: str
    name: str
    state: Literal[
        "ready",
        "unavailable",
        "unknown",
        "unconfigured",
        "configured",
        "not_integrated",
        "disabled",
    ]
    detail: str


class ServiceReport(BaseModel):
    checked_at: float
    services: list[ServiceState]


def local_services(engine, root: Path) -> list[ServiceState]:
    services = [
        ServiceState(id="api", name="本机 API", state="ready", detail="工作台请求已到达本机服务")
    ]
    try:
        with engine.connect() as conn:
            conn.execute(text("SELECT 1"))
        db = "ready"
    except SQLAlchemyError:
        db = "unavailable"
    services.append(
        ServiceState(
            id="database",
            name="数据库",
            state=db,
            detail="数据库查询正常" if db == "ready" else "数据库查询失败",
        )
    )
    worker = "unknown"
    detail = "未发现桌面运行状态，无法确认任务执行器状态"
    try:
        status = json.loads((root.parent / "runtime-status.json").read_text())
        age = time.time() - status["observed_at"]
        if 0 <= age < 5 and status.get("worker") in {"running", "stopped"}:
            worker = "ready" if status["worker"] == "running" else "unavailable"
            detail = (
                "任务执行器进程运行中；不代表任务一定执行成功"
                if worker == "ready"
                else "任务执行器进程已停止"
            )
        else:
            detail = "运行状态已过期或无效，无法确认任务执行器状态"
    except (OSError, ValueError, TypeError, KeyError):
        pass
    services.append(ServiceState(id="worker", name="任务执行器", state=worker, detail=detail))
    return services
