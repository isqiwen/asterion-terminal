"""One local SimNow session; credentials remain in memory only."""

import math
import os
import threading
import time
from datetime import datetime
from typing import cast
from zoneinfo import ZoneInfo

from asterion.market.models import ConnectionState, MarketConfiguration, MarketState, Quote


def number(value, nonnegative=False):
    if not isinstance(value, (float, int)) or not math.isfinite(value) or abs(value) >= 1e100:
        return None
    return None if nonnegative and value < 0 else value


class MarketService:
    def __init__(self, root, factory, clock=time.time):
        self.path = root / "market" / "simnow.json"
        self.factory, self.clock = factory, clock
        self.lock, self.operation = threading.RLock(), threading.Lock()
        self.feed = None
        self.generation = 0
        self.state, self.detail = "disconnected", "行情源未连接"
        self.started = 0
        self.quotes, self.errors = {}, {}
        self.load_error = False
        try:
            self.configuration = (
                MarketConfiguration.model_validate_json(self.path.read_text())
                if self.path.exists()
                else MarketConfiguration()
            )
        except (ValueError, OSError):
            self.configuration = MarketConfiguration()
            self.load_error = True
            self.state, self.detail = (
                "error",
                "行情配置无法读取，原始文件已保留；请检查当前配置格式",
            )

    def configure(self, configuration):
        with self.operation, self.lock:
            if self.load_error:
                raise ValueError("行情配置无法读取，原始文件已保留；请检查当前配置格式")
            if self.feed is not None:
                raise ValueError("请先断开行情，再修改连接或自选")
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_suffix(".tmp")
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as file:
                file.write(configuration.model_dump_json())
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, self.path)
            self.configuration = configuration
            self.quotes.clear()
            self.errors.clear()
            self.state, self.detail = "disconnected", "配置已保存，尚未连接"
        return self.snapshot()

    def connect(self, password):
        with self.operation:
            self._disconnect()
            with self.lock:
                if self.load_error:
                    raise ValueError("行情配置无法读取，原始文件已保留；请检查当前配置格式")
                if not self.configuration.front or not self.configuration.user_id:
                    raise ValueError("请先保存 SimNow 行情前置和投资者代码")
                generation = self.generation
                self.state, self.detail = "connecting", "正在连接 SimNow"
                self.started = self.clock()
                self.errors.clear()
            try:
                feed = self.factory(
                    self.configuration,
                    password,
                    lambda event, value: self.event(generation, event, value),
                )
                self.feed = feed
                feed.start()
            except Exception:  # noqa: BLE001 - native adapter failures must not expose credentials
                self._disconnect()
                with self.lock:
                    self.state, self.detail = "error", "行情 SDK 启动失败，请检查本机运行组件后重连"
        return self.snapshot()

    def _disconnect(self):
        with self.lock:
            self.generation += 1
            feed, self.feed = self.feed, None
            self.state, self.detail = "disconnected", "行情已断开；保留的报价不再实时更新"
            for quote in self.quotes.values():
                quote.stale = True
        if feed is not None:
            feed.close()

    def disconnect(self):
        with self.operation:
            self._disconnect()
        return self.snapshot()

    def event(self, generation, event, value):
        with self.lock:
            if generation != self.generation:
                return
            if event == "subscription_error":
                self.errors[value[0]] = value[1]
                return
            if event != "tick":
                self.state = event
                self.detail = {
                    "connecting": "已连接前置，正在登录",
                    "connected": "SimNow 已登录；报价状态按合约显示",
                    "reconnecting": "行情连接中断，正在自动重连",
                }.get(event, value or "行情错误")
                self.started = self.clock()
                if event != "connected":
                    for quote in self.quotes.values():
                        quote.stale = True
                return
            if self.state != "connected":
                return
            symbol = value["InstrumentID"]
            subscription = next(
                (s for s in self.configuration.subscriptions if s.symbol == symbol), None
            )
            if not subscription:
                return
            if value["ExchangeID"] and value["ExchangeID"] != subscription.exchange:
                self.errors[symbol] = "报价交易所与自选声明不一致，已拒绝"
                return
            event_at = None
            try:
                if not 0 <= value["UpdateMillisec"] <= 999:
                    raise ValueError()
                event_at = (
                    datetime.strptime(value["ActionDay"] + value["UpdateTime"], "%Y%m%d%H:%M:%S")
                    .replace(tzinfo=ZoneInfo("Asia/Shanghai"))
                    .timestamp()
                    + value["UpdateMillisec"] / 1000
                )
            except (ValueError, TypeError):
                pass
            previous = self.quotes.get(symbol)
            if (
                previous
                and previous.event_at is not None
                and event_at is not None
                and event_at < previous.event_at
            ):
                return
            last, settlement = (
                number(value["LastPrice"], True),
                number(value["PreSettlementPrice"], True),
            )
            self.quotes[symbol] = Quote(
                exchange=subscription.exchange,
                symbol=symbol,
                last=last,
                change_percent=(last / settlement - 1) * 100
                if last is not None and settlement
                else None,
                volume=value["Volume"]
                if type(value["Volume"]) is int and value["Volume"] >= 0
                else None,
                open_interest=number(value["OpenInterest"], True),
                trading_day=value["TradingDay"],
                source_time=value["UpdateTime"],
                event_at=event_at,
                received_at=self.clock(),
                stale=False,
            )

    def snapshot(self):
        with self.lock:
            now = self.clock()
            if self.state in {"connecting", "reconnecting"} and now - self.started > 20:
                self.detail = "连接等待超过 20 秒，请核对地址、网络和 SimNow 服务时段；可断开后重试"
            quotes = [
                q.model_copy(
                    update={
                        "stale": q.stale
                        or self.state != "connected"
                        or q.event_at is None
                        or not -5 <= now - q.event_at <= 30
                        or now - q.received_at > 30
                    }
                )
                for q in self.quotes.values()
            ]
            return MarketState(
                state=cast(ConnectionState, self.state),
                detail=self.detail,
                configuration=self.configuration.model_copy(deep=True),
                quotes=quotes,
                subscription_errors=dict(self.errors),
                observed_at=now,
            )
