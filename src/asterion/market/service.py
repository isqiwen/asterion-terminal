"""Market-owned watchlists and quote views, isolated by source connection."""

import threading
import time

from asterion_bindings.files import FilePublicationError, atomic_write
from asterion_bindings.market_quotes import MarketQuotes

from asterion.market.models import MarketState, Watchlist


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
        self.feed = MarketQuotes()
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
        self.feed.subscriptions(self.configuration.subscriptions)
        self.bind()

    def bind(self):
        profile = self.access.profile(self.connection_id)
        if profile.config_revision == self.revision:
            return
        if profile.config_revision != self.revision:
            self.directory.bind(profile)
            self.revision = profile.config_revision
            self.feed.bind(profile.config_revision)
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
            self.path.parent.mkdir(parents=True, exist_ok=True)
            publication_error = None
            try:
                atomic_write(self.path, body.model_dump_json().encode())
            except FilePublicationError as error:
                publication_error = error
            with self.lock:
                self.configuration = body
                self.feed.subscriptions(body.subscriptions)
            self.access.subscribe(self.connection_id, body.subscriptions)
            if publication_error is not None:
                raise publication_error
            return self.snapshot()

    def event(self, generation, kind, value):
        with self.lock:
            channel = self.access.channel(self.connection_id, "market")
            self.feed.event(generation, channel, kind, value, self.clock())

    def snapshot(self):
        with self.lock:
            now = self.clock()
            profile = self.access.profile(self.connection_id)
            channel = self.access.channel(self.connection_id, "market")
            state = self.feed.snapshot(channel, now)
            return MarketState(
                connection_id=self.connection_id,
                connection_name=profile.name,
                state="connected" if channel.state == "ready" else channel.state,
                detail="自选文件不受支持，原文件已保留" if self.load_error else channel.detail,
                configuration=self.configuration.model_copy(deep=True),
                quotes=state["quotes"],
                subscription_errors=state["subscription_errors"],
                contract_names=self.directory.names(),
                observed_at=now,
            )
