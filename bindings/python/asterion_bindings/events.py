"""Payload conversion and restricted publication ports for the native journal."""

import json
from dataclasses import dataclass

from pydantic import BaseModel

from . import _native
from .communication import context, current
from .database import database_identity, kernel_call, native_connection
from .storage import Transaction, transaction_connection


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def descriptor(topic):
    return {
        "id": topic.id,
        "owner": topic.owner,
        # Registry-local exact identity, including dynamically declared types.
        "payload": str(id(topic.payload)),
        "read_path": topic.read_path,
    }


@dataclass(frozen=True)
class Topic:
    id: str
    owner: str
    payload: type[BaseModel]
    read_path: str

    def __post_init__(self):
        _native.event_topic_validate(encoded(descriptor(self)))


class Registry:
    def __init__(self, engine, topics):
        self._topics = tuple(topics)
        self._native = _native.EventRegistry(
            database_identity(engine), encoded([descriptor(topic) for topic in self._topics])
        )

    def check_topic(self, topic):
        self._native.check_topic(encoded(descriptor(topic)))

    def publisher(self, owner, topics):
        topics = tuple(topics)
        return EventPort(
            self._native.publisher(owner, encoded([descriptor(t) for t in topics])), topics
        )

    def read_path(self, topic, method):
        return self._native.read_path(topic, method)

    def publish_host(self, connection, topic, stream, payload, trace):
        return json.loads(
            kernel_call(
                connection,
                self._native.publish_host,
                native_connection(connection),
                encoded(descriptor(topic)),
                stream,
                encoded(payload),
                encoded(trace),
            )
        )

    def read_plan(self, topic, after, limit):
        return self._native.read_plan(encoded({"topic": topic, "after": after, "limit": limit}))

    def read(self, connection, plan):
        return json.loads(
            kernel_call(connection, self._native.read, native_connection(connection), plan)
        )


@dataclass(frozen=True)
class EventPort:
    _grant: _native.EventPublisher
    # Preserve exact payload-class identity for the native registry's lifetime.
    _topics: tuple[Topic, ...]

    def _guarded(self, handle):
        return EventPort(self._grant.guarded(handle), self._topics)

    def publish(self, transaction, topic, stream, payload):
        if not isinstance(transaction, Transaction):
            raise TypeError("Storage transaction is closed or invalid")
        self._grant.check(transaction._handle, encoded(descriptor(topic)))
        payload = topic.payload.model_validate(payload).model_dump(mode="json")
        trace = current() or context()
        return json.loads(
            kernel_call(
                transaction_connection(transaction),
                self._grant.publish,
                transaction._handle,
                encoded(descriptor(topic)),
                stream,
                encoded(payload),
                encoded(trace),
            )
        )


def validate_journal(connection):
    # A host caller may give an idle SQLAlchemy connection; explicitly begin on
    # that connection so validation shares its transaction and never commits it.
    if not connection.in_transaction():
        connection.begin()
    return kernel_call(connection, _native.event_journal_validate, native_connection(connection))
