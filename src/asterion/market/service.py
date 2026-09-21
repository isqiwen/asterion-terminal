"""Market-owned watchlists and quote views, isolated by source connection."""

import os
import threading
import time
from decimal import Decimal


def atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as file:
        file.write(content)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temp, path)


from asterion.market.models import MarketState, Quote, Watchlist


class MarketService:
    def __init__(self, root, connection_id, access, directory, clock=time.time):
        self.connection_id, self.access, self.directory, self.clock = (
            connection_id,
            access,
            directory,
            clock,
        )
        self.path = root / "watchlist.json"
        self.lock, self.operation = threading.RLock(), threading.RLock()
        self.quotes, self.errors = {}, {}
        self.load_error = False
        self.revision = None
        try:
            self.configuration = (
                Watchlist.model_validate_json(self.path.read_text())
                if self.path.exists()
                else Watchlist()
            )
        except (ValueError, OSError):
            self.configuration = Watchlist()
            self.load_error = True
        self.bind()

    def bind(self):
        profile = self.access.profile(self.connection_id)
        if profile.config_revision == self.revision:
            return
        if profile.config_revision != self.revision:
            self.directory.bind(profile)
            self.revision = profile.config_revision
            self.quotes.clear()
            self.errors.clear()
        if self.access.supports(self.connection_id, "market_quotes"):
            self.access.subscribe(self.connection_id, self.configuration.subscriptions)

    def maintain_contracts(self):
        with self.operation:
            self.bind()
            self.expire_contracts()
            if (
                self.access.channel(self.connection_id, "account").state != "disconnected"
                and self.directory.refresh_due()
                and self.access.supports(self.connection_id, "instrument_catalog")
            ):
                self.refresh_contracts()

    def refresh_contracts(self):
        self.directory.request_refresh(self.access.profile(self.connection_id))

    def expire_contracts(self):
        with self.operation:
            if self.load_error:
                return
            expired = self.directory.expired(self.configuration.subscriptions)
            retained = [
                s for s in self.configuration.subscriptions if (s.exchange, s.symbol) not in expired
            ]
            if len(retained) != len(self.configuration.subscriptions):
                self.watchlist(Watchlist(subscriptions=retained))

    def watchlist(self, body):
        with self.operation:
            if self.load_error:
                raise ValueError("自选配置无法读取，原文件已保留")
            self.directory.validate(body, self.configuration.subscriptions)
            expired = self.directory.expired(body.subscriptions)
            if any((s.exchange, s.symbol) in expired for s in body.subscriptions):
                raise ValueError("合约已经到期，请刷新自选")
            atomic(self.path, body.model_dump_json())
            with self.lock:
                self.configuration = body
                retained = {(s.exchange, s.symbol) for s in body.subscriptions}
                self.quotes = {
                    key: q for key, q in self.quotes.items() if (q.exchange, q.symbol) in retained
                }
                self.errors.clear()
            self.access.subscribe(self.connection_id, body.subscriptions)
            return self.snapshot()

    def event(self, generation, kind, value):
        with self.lock:
            if generation != self.access.channel(self.connection_id, "market").generation:
                return
            if kind == "subscription_error":
                self.errors[value[0]] = value[1]
                return
            if kind != "tick" or self.access.channel(self.connection_id, "market").state != "ready":
                return
            subscription = next(
                (s for s in self.configuration.subscriptions if s.symbol == value.symbol), None
            )
            if not subscription:
                return
            if value.exchange and value.exchange != subscription.exchange:
                self.errors[value.symbol] = "来源交易所与自选不一致"
                return
            previous = self.quotes.get(value.symbol)
            if (
                previous
                and previous.event_at is not None
                and previous.event_at <= self.clock() + 5
                and value.event_at is not None
                and value.event_at < previous.event_at
            ):
                return
            base = value.previous_settlement
            change = (
                (Decimal(str(value.last)) - Decimal(str(base)))
                if value.last is not None and base is not None and base > 0
                else None
            )
            self.quotes[value.symbol] = Quote(
                exchange=subscription.exchange,
                symbol=value.symbol,
                last=value.last,
                previous_settlement=base,
                change=float(change) if change is not None else None,
                change_percent=float(change / Decimal(str(base)) * 100)
                if change is not None
                else None,
                high=value.high,
                low=value.low,
                volume=value.volume,
                open_interest=value.open_interest,
                trading_day=value.trading_day,
                action_day=value.action_day,
                source_time=value.source_time,
                event_at=value.event_at,
                received_at=value.received_at,
                stale=False,
            )

    def snapshot(self):
        with self.lock:
            now = self.clock()
            profile = self.access.profile(self.connection_id)
            channel = self.access.channel(self.connection_id, "market")

            def status(q):
                if channel.state != "ready":
                    return "disconnected"
                if q.event_at is None:
                    return "time_unknown"
                if q.event_at > now + 5:
                    return "time_ahead"
                if now - q.received_at > 30:
                    return "not_updated"
                if now - q.event_at > 30:
                    return "delayed"
                return "current"

            quotes = [
                q.model_copy(update={"status": status(q), "stale": status(q) != "current"})
                for q in self.quotes.values()
            ]
            return MarketState(
                connection_id=self.connection_id,
                connection_name=profile.name,
                state="connected" if channel.state == "ready" else channel.state,
                detail="自选文件不受支持，原文件已保留" if self.load_error else channel.detail,
                configuration=self.configuration.model_copy(deep=True),
                quotes=quotes,
                subscription_errors=dict(self.errors),
                contract_names=self.directory.names(),
                observed_at=now,
            )
