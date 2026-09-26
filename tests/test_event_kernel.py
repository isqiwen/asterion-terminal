# ruff: noqa: F811
"""Native event admission with the existing SQL transaction and restored rows."""

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.events import Registry, Topic, validate_journal
from asterion_bindings.storage import Storage
from pydantic import BaseModel, create_model
from sqlalchemy import select
from test_communication import Fact, event_store  # noqa: F401

from asterion.platform.communication.events import (
    EventJournal,
    events,
    heads,
)


@pytest.mark.parametrize(
    "after,limit", [("latest", 0), ("latest", 501), ("0", True), (0, 1), ("１", 1)]
)
def test_read_plan_is_validated_before_latest_or_database_queries(event_store, after, limit):
    _engine, _owned, topic, journal = event_store
    with pytest.raises(ValueError):
        journal.read(topic.id, after, limit)


def test_registered_payload_identity_and_read_authorization_are_not_interchangeable(event_store):
    _engine, _owned, topic, journal = event_store
    other_type = create_model(Fact.__name__, value=(int, ...), __module__=Fact.__module__)
    substituted = Topic(topic.id, topic.owner, other_type, topic.read_path)
    with pytest.raises(ValueError):
        journal.publisher(topic.owner, (substituted,))
    registry = Registry(_engine, (topic,))
    assert registry.read_path(topic.id, "GET") == "/fixture"
    for identifier, method in [(topic.id, "POST"), ("missing.topic", "GET")]:
        with pytest.raises(ValueError):
            registry.read_path(identifier, method)


def test_payload_failure_rolls_back_business_write_without_creating_a_head(event_store):
    engine, owned, _topic, _journal = event_store

    class Text(BaseModel):
        text: str

    topic = Topic("fixture.large", "fixture.owner", Text, "/fixture")
    journal = EventJournal(engine, (topic,))
    store = Storage(engine, (owned,))
    port = journal.publisher(topic.owner, (topic,))
    with pytest.raises(ValueError, match="64 KiB"), store.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        port.publish(transaction, topic, "one", {"text": "期" * 21842})
    with engine.connect() as conn:
        assert conn.execute(select(owned)).all() == []
        assert conn.execute(select(heads)).all() == []
        assert conn.execute(select(events)).all() == []


@pytest.mark.parametrize("bad_head", [-1, 9223372036854775807])
def test_guarded_bigint_allocation_refuses_invalid_heads_without_mutation(event_store, bad_head):
    engine, _, topic, journal = event_store
    with engine.begin() as conn:
        conn.execute(heads.insert().values(topic=topic.id, sequence=bad_head))
    with pytest.raises(ValueError, match="sequence"), engine.begin() as conn:
        journal.publish(conn, topic, "one", {"value": 1})
    with engine.connect() as conn:
        assert conn.execute(select(heads.c.sequence)).scalar_one() == bad_head
        assert conn.execute(select(events)).all() == []


@pytest.mark.parametrize(
    "field,value", [("owner", "other.owner"), ("stream", "other"), ("sequence", "2")]
)
def test_replay_refuses_corrupt_envelopes_instead_of_advancing_the_cursor(
    event_store, field, value
):
    engine, _, topic, journal = event_store
    with engine.begin() as conn:
        message = journal.publish(conn, topic, "one", {"value": 1})
    damaged = message | {field: value}
    with engine.begin() as conn:
        conn.execute(events.update().values(message=damaged))
    with pytest.raises(ValueError):
        journal.read(topic.id)
    with engine.connect() as conn:
        assert conn.execute(select(events.c.message)).scalar_one() == damaged
        assert conn.execute(select(heads.c.sequence)).scalar_one() == 1


def test_foreign_and_inactive_transactions_cannot_publish(event_store):
    engine, _, topic, journal = event_store
    with engine.connect() as conn, pytest.raises(ValueError, match="transaction"):
        journal.publish(conn, topic, "one", {"value": 1})
    foreign = create_engine("sqlite://")
    try:
        with foreign.begin() as conn, pytest.raises(ValueError, match="transaction"):
            journal.publish(conn, topic, "one", {"value": 1})
    finally:
        foreign.dispose()


def test_restore_checks_all_rows_and_heads_without_mutating_facts(event_store):
    engine, _, topic, journal = event_store
    with engine.begin() as conn:
        for number in range(1005):
            journal.publish(conn, topic, str(number), {"value": number})
    with engine.connect() as conn:
        assert validate_journal(conn) == 1005
    with engine.begin() as conn:
        conn.execute(events.update().where(events.c.sequence == 1005).values(stream="damaged"))
    with engine.connect() as conn, pytest.raises(ValueError, match="inconsistent"):
        validate_journal(conn)
    with engine.connect() as conn:
        assert (
            conn.execute(select(events.c.stream).where(events.c.sequence == 1005)).scalar_one()
            == "damaged"
        )
        assert conn.execute(select(heads.c.sequence)).scalar_one() == 1005


def test_joined_event_writer_revocation_rolls_back_all_participants(event_store):
    engine, owned, topic, journal = event_store
    owner = Storage(engine, (owned,))
    participant = Storage(engine, ())
    port = journal.publisher(topic.owner, (topic,))
    with pytest.raises(ValueError, match="closed"), owner.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        with participant.join(transaction) as joined:
            port.publish(joined, topic, "one", {"value": 1})
        participant.close()
    with engine.connect() as conn:
        assert conn.execute(select(owned)).all() == []
        assert conn.execute(select(events)).all() == []
        assert conn.execute(select(heads)).all() == []


def test_caught_native_publication_failure_cannot_commit_business(event_store):
    engine, owned, _topic, _journal = event_store

    class Text(BaseModel):
        text: str

    topic = Topic("fixture.large", "fixture.owner", Text, "/fixture")
    port = EventJournal(engine, (topic,)).publisher(topic.owner, (topic,))
    store = Storage(engine, (owned,))
    with pytest.raises(ValueError, match="closed"), store.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        with pytest.raises(ValueError, match="64 KiB"):
            port.publish(transaction, topic, "one", {"text": "期" * 21842})
    with engine.connect() as conn:
        assert conn.execute(select(owned)).all() == []
        assert conn.execute(select(events)).all() == []
        assert conn.execute(select(heads)).all() == []


def test_scope_cannot_be_reused_after_its_physical_transaction_changes(event_store):
    from asterion_bindings.database import native_connection
    from asterion_bindings.storage import transaction_connection

    engine, owned, topic, journal = event_store
    store = Storage(engine, (owned,))
    port = journal.publisher(topic.owner, (topic,))
    with pytest.raises(ValueError, match="closed"), store.begin() as transaction:
        physical = native_connection(transaction_connection(transaction))
        physical.rollback()
        physical.begin()
        with pytest.raises(ValueError, match="closed"):
            port.publish(transaction, topic, "one", {"value": 1})
    assert journal.read(topic.id)["items"] == []


def test_publisher_retains_exact_payload_type_after_journal_is_released(event_store):
    import gc
    import weakref

    engine, _owned, _topic, _journal = event_store
    payload_type = create_model("EphemeralFact", value=(int, ...))
    reference = weakref.ref(payload_type)
    topic = Topic("fixture.ephemeral", "fixture.owner", payload_type, "/fixture")
    journal = EventJournal(engine, (topic,))
    port = journal.publisher(topic.owner, (topic,))
    del payload_type, topic, journal
    gc.collect()
    assert reference() is not None
    with Storage(engine, ()).begin() as transaction:
        topic = Topic("fixture.ephemeral", "fixture.owner", reference(), "/fixture")
        assert port.publish(transaction, topic, "one", {"value": 1})["sequence"] == "1"


def test_plugin_publication_lifecycle_revokes_callbacks_and_uncommitted_events(event_store):
    from asterion_bindings.plugin_host import Activation, Plugin, PluginHost
    from fastapi import FastAPI

    engine, owned, topic, journal = event_store
    store = Storage(engine, (owned,))
    callbacks = []

    def activate(context):
        callbacks.append(context.publish)
        # Activation owns a live native context before the plugin is published.
        with store.begin() as transaction:
            context.publish(transaction, topic, "activation", {"value": 0})
        return Activation()

    host = PluginHost((Plugin(topic.owner, (), activate, publishes=(topic,)),))
    host.activate(FastAPI(), {}, events=journal)
    with pytest.raises(ValueError, match="closed"), store.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        callbacks[0](transaction, topic, "pending", {"value": 1})
        host.close()
    with store.begin() as transaction, pytest.raises(ValueError, match="closed"):
        callbacks[0](transaction, topic, "revoked", {"value": 2})
    assert [item["stream"] for item in journal.read(topic.id)["items"]] == ["activation"]
    with engine.connect() as conn:
        assert conn.execute(select(owned)).all() == []


def test_revoked_or_undeclared_publication_never_runs_payload_validators(event_store):
    from asterion_bindings.plugin_host import Activation, Plugin, PluginHost
    from fastapi import FastAPI
    from pydantic import field_validator

    engine, _owned, _topic, _journal = event_store
    validated = []

    class Payload(BaseModel):
        value: int

        @field_validator("value")
        @classmethod
        def observe(cls, value):
            validated.append(value)
            return value

    class AnotherPayload(Payload):
        pass

    topic = Topic("fixture.admission", "fixture.owner", Payload, "/fixture")
    wrong = Topic(topic.id, topic.owner, AnotherPayload, topic.read_path)
    journal = EventJournal(engine, (topic,))
    port = journal.publisher(topic.owner, (topic,))
    store = Storage(engine, ())
    with store.begin() as transaction, pytest.raises(ValueError, match="Undeclared"):
        port.publish(transaction, wrong, "wrong", {"value": 1})
    callbacks = []

    def activate(context):
        callbacks.append(context.publish)
        return Activation()

    host = PluginHost((Plugin(topic.owner, (), activate, publishes=(topic,)),))
    host.activate(FastAPI(), {}, events=journal)
    host.close()
    with store.begin() as transaction, pytest.raises(ValueError, match="closed"):
        callbacks[0](transaction, topic, "revoked", {"value": 2})
    assert validated == []
    assert journal.read(topic.id)["items"] == []
