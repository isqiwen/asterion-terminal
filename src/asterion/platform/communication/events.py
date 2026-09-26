"""Transactional event journal with per-topic commit ordering and bounded replay."""

from types import MappingProxyType

from asterion_bindings.communication import context, current
from asterion_bindings.events import Registry
from sqlalchemy import (
    JSON,
    BigInteger,
    Column,
    Index,
    String,
    Table,
    UniqueConstraint,
)

from asterion.platform.store import metadata

heads = Table(
    "communication_heads",
    metadata,
    Column("topic", String, primary_key=True),
    Column("sequence", BigInteger, nullable=False),
)
events = Table(
    "communication_events",
    metadata,
    Column("id", String, primary_key=True),
    Column("topic", String, nullable=False),
    Column("sequence", BigInteger, nullable=False),
    Column("stream", String, nullable=False),
    Column("message", JSON, nullable=False),
    UniqueConstraint("topic", "sequence"),
)
Index("communication_stream", events.c.topic, events.c.stream, events.c.sequence)
TABLES = (heads, events)


class EventJournal:
    def __init__(self, engine, topics=()):
        self.engine = engine
        topics = tuple(topics)
        self._rules = Registry(engine, topics)
        self.topics = MappingProxyType({topic.id: topic for topic in topics})

    def publish(self, conn, topic, stream, payload, *, trace=None):
        if conn.engine is not self.engine or not conn.in_transaction():
            raise ValueError("Event requires its database transaction")
        self._rules.check_topic(topic)
        payload = topic.payload.model_validate(payload).model_dump(mode="json")
        trace = trace if trace is not None else current() or context()
        return self._rules.publish_host(conn, topic, stream, payload, trace)

    def read(self, topic, after="0", limit=100):
        plan = self._rules.read_plan(topic, after, limit)
        with self.engine.connect() as conn, conn.begin():
            return self._rules.read(conn, plan)

    def publisher(self, owner, topics):
        return self._rules.publisher(owner, tuple(topics))
