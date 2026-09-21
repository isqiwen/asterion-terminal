"""Account views consume normalized observations; no vendor fields or SDK."""

import threading
import time
from decimal import Decimal
from typing import Literal

from pydantic import BaseModel, Field

from asterion.connections.public import AccountSummary, ConnectorError, Position


class AccountState(BaseModel):
    connection_id: str
    connection_name: str
    state: Literal["disconnected", "loading", "ready", "error"]
    detail: str
    stale: bool = True
    started_at: float | None = None
    observed_at: float | None = None
    account: AccountSummary | None = None
    positions: list[Position] = Field(default_factory=list)
    margin_ratio: Decimal | None = None
    available_ratio: Decimal | None = None
    risk_detail: str = "未接入完整风控规则；敞口、集中度及总体风险暂无法评估"


class AccountService:
    def __init__(self, access, connection_id, names, clock=time.time):
        self.access, self.connection_id, self.names, self.clock = (
            access,
            connection_id,
            names,
            clock,
        )
        self.lock, self.operation = threading.RLock(), threading.Lock()
        self.cancel = threading.Event()
        self.session = None
        self.last_attempt = 0
        self.state = self.empty()

    def empty(self):
        p = self.access.profile(self.connection_id)
        return AccountState(
            connection_id=p.connection_id,
            connection_name=p.name,
            state="disconnected",
            detail="尚未连接；请在连接设置中选择配置并连接",
        )

    def current(self):
        channel = self.access.channel(self.connection_id, "account")
        return channel.generation if channel.state != "disconnected" else None

    def snapshot(self):
        with self.lock:
            if self.session != self.current() or self.session is None:
                return self.empty()
            result = self.state.model_copy(deep=True)
            result.stale = (
                result.state == "error"
                or result.observed_at is None
                or self.clock() - result.observed_at > 60
            )
            return result

    def refresh(self, *, force=False):
        if not self.operation.acquire(blocking=False):
            return self.snapshot()
        try:
            session = self.current()
            if session is None:
                return self.snapshot()
            with self.lock:
                if not force and session == self.session and self.clock() - self.last_attempt < 30:
                    return self.snapshot()
                if session != self.session:
                    self.state = self.empty()
                self.session = session
                self.last_attempt = self.clock()
                self.state.state, self.state.detail = "loading", "正在查询账户与持仓"
            try:
                batch = self.access.account(self.connection_id, self.cancel)
                if session != self.current() or self.cancel.is_set():
                    return self.snapshot()
                names = self.names(self.connection_id)
                account = batch.account
                with self.lock:
                    self.state = self.empty().model_copy(
                        update={
                            "state": "ready",
                            "detail": "只读账户 · 每 30 秒查询；资金与持仓为顺序查询快照",
                            "stale": False,
                            "started_at": batch.started_at,
                            "observed_at": batch.observed_at,
                            "account": account,
                            "positions": [
                                p.model_copy(update={"name": names.get(f"{p.exchange}.{p.symbol}")})
                                for p in batch.positions
                            ],
                            "margin_ratio": account.margin / account.balance * 100
                            if account.margin is not None
                            and account.balance is not None
                            and account.balance > 0
                            else None,
                            "available_ratio": account.available / account.balance * 100
                            if account.available is not None
                            and account.balance is not None
                            and account.balance > 0
                            else None,
                        }
                    )
            except Exception as error:  # noqa: BLE001
                with self.lock:
                    self.state.state = "error"
                    self.state.detail = (
                        str(error)
                        if isinstance(error, ConnectorError)
                        else "账户查询未通过校验，请重试"
                    )
            return self.snapshot()
        finally:
            self.operation.release()
