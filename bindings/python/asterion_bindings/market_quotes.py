"""Value conversion for the connection-scoped native quote cache."""

import json

from . import _native


class MarketQuotes:
    def __init__(self):
        self._native = _native.MarketQuotes()

    def bind(self, revision):
        self._native.bind(revision)

    def subscriptions(self, values):
        self._native.subscriptions(json.dumps([value.model_dump(mode="json") for value in values]))

    def event(self, generation, channel, kind, value, now):
        if kind == "tick":
            self._native.ingest(
                generation,
                channel.generation,
                channel.state == "ready",
                value.model_dump_json(),
                now,
            )
        elif kind == "subscription_error":
            self._native.subscription_error(generation, channel.generation, *value)
        else:
            self._native.connection_event(generation, channel.generation, kind)

    def snapshot(self, channel, now):
        return json.loads(self._native.snapshot(channel.generation, channel.state == "ready", now))
