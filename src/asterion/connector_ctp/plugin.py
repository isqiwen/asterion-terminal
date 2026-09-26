"""Trusted CTP protocol contribution; no business panels or execution capability."""

import re
import threading
import time
from types import SimpleNamespace
from typing import cast

from asterion_bindings.plugin_host import Activation, Plugin

from asterion.connections.public import (
    AccountBatch,
    ConfigField,
    ConnectorContribution,
    ConnectorDescriptor,
    ConnectorError,
    InstrumentBatch,
)

from .feed import CtpFeed
from .normalize import observation, quote
from .query import query_account, query_instruments


def validate(config, secrets):
    for key in ("front", "trade_front"):
        match = re.fullmatch(r"tcp://([A-Za-z0-9.-]+):([0-9]{1,5})", config.get(key, ""))
        if not match or not 1 <= int(match[2]) <= 65535:
            raise ValueError("前置格式应为 tcp://主机:端口")
    for key, limit in (("broker_id", 10), ("user_id", 15), ("app_id", 32)):
        if not config.get(key) or len(config[key]) > limit or not config[key].isascii():
            raise ValueError("经纪商、投资者或应用标识无效")
    if any(
        not secrets.get(key) or len(secrets[key]) > limit
        for key, limit in (("password", 40), ("auth_code", 64))
    ):
        raise ValueError("请填写账户密码和认证码")


class Session:
    def __init__(self, profile, secrets):
        validate(profile.config, secrets)
        self.profile = profile
        self.configuration = SimpleNamespace(
            **profile.config, auth_code=secrets["auth_code"], subscriptions=[]
        )
        self.password = secrets["password"]
        self.feed = None
        self.query_lock = threading.Lock()
        self.next_query = 0.0
        self.closed = threading.Event()
        self.market_lock = threading.RLock()

    def start_market(self, subscriptions, emit):
        with self.market_lock:
            self.stop_market()
            self.configuration.subscriptions = subscriptions

            def event(kind, value):
                if self.closed.is_set():
                    return
                try:
                    emit(kind, quote(value) if kind == "tick" else value)
                except (ValueError, TypeError, KeyError):
                    emit("subscription_warning", "来源报价未通过校验")

            self.feed = CtpFeed(self.configuration, self.password, event)
            self.feed.start()

    def update_subscriptions(self, subscriptions):
        with self.market_lock:
            if self.feed:
                self.feed.update_subscriptions(subscriptions)

    def stop_market(self):
        with self.market_lock:
            feed, self.feed = self.feed, None
            if feed:
                feed.close()

    def read(self, kind, request, cancel):
        while not self.query_lock.acquire(timeout=0.1):
            if cancel.is_set() or self.closed.is_set():
                raise ConnectorError("查询已取消", "session_invalid", False)
        try:
            if cancel.is_set() or self.closed.is_set():
                raise ConnectorError("查询已取消", "session_invalid", False)
            while time.monotonic() < self.next_query:
                if cancel.is_set() or self.closed.wait(0.05):
                    raise ConnectorError("查询已取消", "session_invalid", False)
            query = query_instruments if kind == "instruments" else query_account
            raw = query(self.configuration, self.password, cancel)
            meta = {
                **request.model_dump(),
                "observed_at": time.time(),
                "complete": True,
            }
            if kind == "instruments":
                return InstrumentBatch(**meta, instruments=raw)
            raw = cast(dict, raw)
            account, positions = observation(raw)
            if raw["accounts"][0]["AccountID"] != self.profile.config["user_id"]:
                raise ConnectorError("账户身份与当前连接不一致", "incomplete", False)
            return AccountBatch(
                **meta,
                source_account_id=raw["accounts"][0]["AccountID"],
                account=account,
                positions=positions,
            )
        finally:
            self.next_query = time.monotonic() + 1.1
            self.query_lock.release()

    def instruments(self, request, cancel):
        return cast(InstrumentBatch, self.read("instruments", request, cancel))

    def account(self, request, cancel):
        return cast(AccountBatch, self.read("account", request, cancel))

    def close(self):
        self.closed.set()
        self.stop_market()
        with self.query_lock:
            self.password = ""
            self.configuration.auth_code = ""


def contribution():
    return ConnectorContribution(
        ConnectorDescriptor(
            id="ctp",
            owner="asterion.connector.ctp",
            title="CTP",
            instructions="请填写服务方提供的 BrokerID、投资者代码、行情和交易前置、AppID、认证码及密码。当前仅开放行情与账户只读查询。",
            capabilities=("market_quotes", "instrument_catalog", "account_snapshot", "positions"),
            fields=(
                ConfigField(key="broker_id", label="BrokerID", identity=True),
                ConfigField(key="user_id", label="投资者代码", identity=True),
                ConfigField(key="front", label="行情前置"),
                ConfigField(key="trade_front", label="交易前置"),
                ConfigField(key="app_id", label="AppID"),
                ConfigField(key="auth_code", label="认证码", secret=True),
                ConfigField(key="password", label="账户密码", secret=True),
            ),
        ),
        validate,
        Session,
    )


plugin = Plugin(
    "asterion.connector.ctp",
    ("asterion.connections",),
    lambda context: Activation(hooks={"connections.connectors": (contribution,)}),
)
