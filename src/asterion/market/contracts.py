"""来源-owned instrument cache and asynchronous, atomic refresh."""

import hashlib
import json
import os
import re
import threading
from datetime import datetime
from zoneinfo import ZoneInfo

from pydantic import AwareDatetime, Field, model_validator

from asterion.connections.public import ConnectorError, SourceInstrument
from asterion.market.models import (
    ContractChoices,
    ContractDirectoryState,
    MarketModel,
)

SHANGHAI = ZoneInfo("Asia/Shanghai")


def display_name(contract: SourceInstrument) -> str:
    """Presentation only; retain source names and identities in the cache."""
    name = contract.name.strip()
    if not name or name.casefold() == contract.symbol.casefold():
        return contract.symbol
    month = contract.delivery_month[2:].replace("-", "")
    # Only remove this contract's month suffix, never digits within a product name.
    suffixes = (
        contract.delivery_month,
        contract.delivery_month.replace("-", ""),
        month,
        contract.symbol[len(contract.product) :],
    )
    for suffix in suffixes:
        if name.endswith(suffix):
            name = name[: -len(suffix)].strip()
            break
    name = re.sub(r"期货$", "", name).strip()
    if contract.exchange == "SHFE" and contract.product.lower() == "ad" and name == "铸造铝合金":
        name = "铸铝"
    return f"{name}{month}" if name else contract.symbol


class InstrumentCache(MarketModel):
    version: int = Field(default=1, ge=1, le=1)
    identity: str
    observed_at: AwareDatetime
    contracts: list[SourceInstrument] = Field(max_length=10000)
    retained_lifecycles: list[SourceInstrument] = Field(default_factory=list, max_length=100000)

    @model_validator(mode="after")
    def unique(self):
        rows = self.contracts + self.retained_lifecycles
        if len({(c.exchange, c.symbol) for c in rows}) != len(rows):
            raise ValueError("合约重复")
        return self


class ContractChoicesService:
    def __init__(self, root, query, clock=lambda: datetime.now(SHANGHAI)):
        self.path = root / "instruments.json"
        self.query, self.clock = query, clock
        self.lock = threading.RLock()
        self.cancel = threading.Event()
        self.last_attempt = None
        self.thread = None
        self.cache = None
        self.identity = ""
        self.state: ContractDirectoryState = "missing"
        self.detail = "连接 来源 后自动获取合约，也可点击重新获取"

    def bind(self, configuration):
        identity = hashlib.sha256(
            json.dumps(
                [configuration.connection_id, configuration.config_revision], ensure_ascii=True
            ).encode()
        ).hexdigest()
        with self.lock:
            if self.identity == identity:
                return
        self.stop()
        with self.lock:
            self.identity, self.cache = identity, None
            self.state, self.detail = "missing", "连接 来源 后自动获取合约，也可点击重新获取"
            if not self.path.exists():
                return
            try:
                envelope = json.loads(self.path.read_text())
                if set(envelope) != {"payload", "checksum"}:
                    raise ValueError("invalid cache")
                payload = envelope["payload"]
                if hashlib.sha256(payload.encode()).hexdigest() != envelope["checksum"]:
                    raise ValueError("cache checksum")
                cache = InstrumentCache.model_validate_json(payload)
                if cache.observed_at > self.clock():
                    raise ValueError("future observation")
                if cache.identity == identity:
                    self.cache = cache
                    self.state, self.detail = "ready", "已载入本机 来源 合约缓存，连接后自动刷新"
            except (ValueError, OSError, TypeError, AttributeError):
                self.state, self.detail = (
                    "invalid",
                    "本机合约缓存校验失败，请重新获取；原文件尚未改动",
                )

    def request_refresh(self, configuration):
        self.bind(configuration)
        with self.lock:
            if self.thread is not None and self.thread.is_alive():
                return
            self.last_attempt = self.clock()
            self.cancel = threading.Event()
            self.state, self.detail = "loading", "正在从 来源 查询合约…"
            self.thread = threading.Thread(
                target=self._refresh,
                args=(),
                name="source-instruments",
                daemon=True,
            )
            self.thread.start()

    def _refresh(self):
        try:
            rows = self.query(self.cancel).instruments
            keys = {(row.exchange, row.symbol) for row in rows}
            with self.lock:
                previous = (
                    (self.cache.contracts + self.cache.retained_lifecycles) if self.cache else []
                )
            cache = InstrumentCache(
                identity=self.identity,
                observed_at=self.clock(),
                contracts=rows,
                retained_lifecycles=[
                    row for row in previous if (row.exchange, row.symbol) not in keys
                ],
            )
            if not cache.contracts:
                raise ValueError("来源 未返回期货合约，请稍后重试")
            payload = cache.model_dump_json()
            content = json.dumps(
                {"payload": payload, "checksum": hashlib.sha256(payload.encode()).hexdigest()}
            )
            with self.lock:
                if self.cancel.is_set():
                    return
                self.path.parent.mkdir(parents=True, exist_ok=True)
                temporary = self.path.with_suffix(".tmp")
                fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "w") as file:
                    file.write(content)
                    file.flush()
                    os.fsync(file.fileno())
                os.replace(temporary, self.path)
                self.cache = cache
                self.state, self.detail = "ready", "来源 合约已更新"
        except Exception as error:  # noqa: BLE001 - preserve complete cache on every query failure
            with self.lock:
                if not self.cancel.is_set():
                    self.state = "error"
                    # Only our query messages are safe; never surface arbitrary SDK errors.
                    self.detail = (
                        str(error)
                        if isinstance(error, ConnectorError)
                        else "合约查询或缓存保存失败，请重新获取"
                    )

    def refresh_due(self):
        with self.lock:
            now = self.clock()
            return (
                self.cache is None
                or self.cache.observed_at.astimezone(SHANGHAI).date() < now.date()
            ) and (self.last_attempt is None or (now - self.last_attempt).total_seconds() >= 300)

    def stop(self):
        with self.lock:
            self.cancel.set()
            thread = self.thread
        if thread is not None:
            thread.join()
        with self.lock:
            self.thread = None
            if self.state == "loading":
                self.state = "ready" if self.cache else "missing"
                self.detail = "合约查询已取消；可重新获取"

    def names(self):
        with self.lock:
            rows = self.cache.contracts + self.cache.retained_lifecycles if self.cache else []
            return {
                f"{c.exchange}.{c.symbol}": display_name(c)
                for c in rows
                if c.name.strip() and c.name.casefold() != c.symbol.casefold()
            }

    def choices(self, exchange):
        now = self.clock()
        with self.lock:
            rows = self.cache.contracts if self.cache else []
            return ContractChoices(
                exchange=exchange,
                as_of=now.date(),
                state=self.state,
                detail=self.detail,
                source="连接合约目录",
                observed_at=self.cache.observed_at if self.cache else None,
                contracts=sorted(
                    [
                        c.model_copy(update={"name": display_name(c)})
                        for c in rows
                        if c.exchange == exchange and c.listed_on <= now.date() <= c.last_trade_on
                    ],
                    key=lambda c: (c.product, c.delivery_month),
                ),
            )

    def expired(self, subscriptions):
        with self.lock:
            return (
                {
                    (c.exchange, c.symbol)
                    for c in self.cache.contracts + self.cache.retained_lifecycles
                    if c.last_trade_on < self.clock().date()
                }
                if self.cache
                else set()
            )

    def validate(self, body, previous):
        retained = {(s.exchange, s.symbol) for s in previous}
        for exchange in {s.exchange for s in body.subscriptions}:
            choices = self.choices(exchange)
            allowed = {c.symbol for c in choices.contracts}
            for row in body.subscriptions:
                if (
                    row.exchange == exchange
                    and (exchange, row.symbol) not in retained
                    and row.symbol not in allowed
                ):
                    raise ValueError("所选合约已到期或不在 来源 合约列表，请重新获取后选择")
