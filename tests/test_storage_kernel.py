"""Native transaction grants are enforced at real database publication boundaries."""

import os
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.storage import Storage, initialize_stores
from asterion_bindings.task_repository import Tasks, task_port
from sqlalchemy import (
    Column,
    Integer,
    MetaData,
    Table,
    event,
    func,
    inspect,
    literal_column,
    select,
)
from sqlalchemy.exc import IntegrityError


def test_revoked_joined_writer_rolls_back_all_participants(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/participants.db")
    schema = MetaData()
    left = Table("left", schema, Column("id", Integer, primary_key=True))
    right = Table("right", schema, Column("id", Integer, primary_key=True))
    schema.create_all(engine)
    a, b = Storage(engine, (left,)), Storage(engine, (right,))
    try:
        with pytest.raises(ValueError, match="closed"), a.begin() as root:
            root.execute(left.insert().values(id=1))
            with b.join(root) as joined:
                joined.execute(right.insert().values(id=2))
            b.close()
        with engine.connect() as conn:
            assert conn.execute(select(left)).first() is None
            assert conn.execute(select(right)).first() is None
    finally:
        engine.dispose()


def test_host_close_revokes_retained_storage_and_rolls_back_uncommitted_rows(tmp_path):
    from asterion_bindings.plugin_host import Activation, Plugin, PluginHost
    from asterion_bindings.resource import Resource
    from fastapi import FastAPI

    engine = create_engine(f"sqlite:///{tmp_path}/host-revocation.db")
    table = Table("owned", MetaData(), Column("id", Integer, primary_key=True))
    table.metadata.create_all(engine)
    store = Storage(engine, (table,))
    resource = Resource("fixture.store", Storage)
    retained = []

    def activate(context):
        retained.append(context.resource(resource))
        return Activation()

    host = PluginHost((Plugin("fixture.owner", (), activate, resources=(resource,)),))
    try:
        host.activate(FastAPI(), {"fixture.owner": {resource: store}})
        owned = retained[0]
        assert isinstance(owned, Storage)
        with pytest.raises(ValueError, match="closed"), owned.begin() as transaction:
            transaction.execute(table.insert().values(id=1))
            host.close()
        with pytest.raises(ValueError, match="closed"), owned.connect():
            pytest.fail("A closed plugin cannot begin a new query")
        with engine.connect() as connection:
            assert connection.execute(select(table)).first() is None
    finally:
        host.close()
        engine.dispose()


def test_execution_context_revokes_owned_storage_and_preserves_closed_port_identity(tmp_path):
    from asterion_bindings.resource import Resource
    from asterion_bindings.tasks import ExecutionContext

    engine = create_engine(f"sqlite:///{tmp_path}/execution-revocation.db")
    table = Table("owned", MetaData(), Column("id", Integer, primary_key=True))
    table.metadata.create_all(engine)
    store = Storage(engine, (table,))
    resource = Resource("fixture.store", Storage)
    execution = ExecutionContext((resource,), {resource: store})
    try:
        owned = execution.resource(resource)
        assert execution.resource(resource) is owned
        with pytest.raises(ValueError, match="closed"), owned.begin() as transaction:
            transaction.execute(table.insert().values(id=1))
            execution.close()
        with engine.connect() as connection:
            assert connection.execute(select(table)).first() is None
    finally:
        execution.close()
        engine.dispose()


def test_separate_table_objects_cannot_claim_same_physical_table():
    engine = create_engine("sqlite://")
    a = Table("shared", MetaData(), Column("id", Integer, primary_key=True))
    b = Table("shared", MetaData(), Column("id", Integer, primary_key=True))
    try:
        with pytest.raises(ValueError, match="ownership"):
            initialize_stores(
                engine,
                {"a": {"store": Storage(engine, (a,))}, "b": {"store": Storage(engine, (b,))}},
            )
    finally:
        engine.dispose()


def test_readonly_handle_cannot_mutate_tasks_through_core_port():
    engine = create_engine("sqlite://")
    store = Storage(engine, ())
    port = task_port(Tasks(engine), frozenset({"fixture.task"}))
    try:
        with store.connect() as conn, pytest.raises(ValueError, match="read-only"):
            port.submit_batch(conn, [("command", "fixture.task", {})])
    finally:
        engine.dispose()


def test_literal_columns_cannot_read_undeclared_tables_but_count_star_works(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/literals.db")
    schema = MetaData()
    owned = Table("owned", schema, Column("id", Integer, primary_key=True))
    secret = Table("secret", schema, Column("id", Integer, primary_key=True))
    schema.create_all(engine)
    store = Storage(engine, (owned,))
    try:
        with engine.begin() as connection:
            connection.execute(secret.insert().values(id=42))
        with store.begin() as connection:
            connection.execute(owned.insert().values(id=1))
            for statement in (
                select(literal_column("(SELECT id FROM secret LIMIT 1)")),
                select(func.count(literal_column("(SELECT id FROM secret LIMIT 1)"))),
                select(literal_column("*")),
            ):
                with pytest.raises(ValueError, match="Raw SQL"):
                    connection.execute(statement)
            assert connection.execute(select(func.count()).select_from(owned)).scalar_one() == 1
    finally:
        store.close()
        engine.dispose()


def test_caught_participant_failure_still_rolls_back_every_writer(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/failed-participant.db")
    schema = MetaData()
    left = Table("left", schema, Column("id", Integer, primary_key=True))
    right = Table("right", schema, Column("id", Integer, primary_key=True))
    schema.create_all(engine)
    a, b = Storage(engine, (left,)), Storage(engine, (right,))
    try:
        with pytest.raises(ValueError, match="closed"), a.begin() as root:
            root.execute(left.insert().values(id=1))
            try:
                with b.join(root) as joined:
                    joined.execute(right.insert().values(id=2))
                    raise RuntimeError("participant failed after writing")
            except RuntimeError:
                pass
        with engine.connect() as connection:
            assert connection.execute(select(left)).first() is None
            assert connection.execute(select(right)).first() is None
    finally:
        a.close()
        b.close()
        engine.dispose()


def test_caught_database_error_cannot_publish_earlier_writes(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/failed-statement.db")
    table = Table("records", MetaData(), Column("id", Integer, primary_key=True))
    store = Storage(engine, (table,))
    try:
        store.initialize(table)
        with pytest.raises(ValueError, match="closed"), store.begin() as connection:
            connection.execute(table.insert().values(id=1))
            with pytest.raises(IntegrityError):
                connection.execute(table.insert().values(id=1))
        with engine.connect() as connection:
            assert connection.execute(select(table)).first() is None
    finally:
        store.close()
        engine.dispose()


@pytest.mark.parametrize("assembled", [False, True])
def test_revocation_during_ddl_prevents_schema_commit(tmp_path, assembled):
    engine = create_engine(f"sqlite:///{tmp_path}/schema-{assembled}.db")
    table = Table("pending", MetaData(), Column("id", Integer, primary_key=True))
    store = Storage(engine, (table,))
    # Fire after the physical CREATE, so this exercises commit admission rather
    # than only checking a grant before the transaction started.
    event.listen(table, "after_create", lambda *_args, **_kwargs: store.close())
    try:
        with pytest.raises(ValueError, match="closed"):
            if assembled:
                initialize_stores(engine, {"fixture": {"storage": store}})
            else:
                store.initialize(table)
        assert "pending" not in inspect(engine).get_table_names()
    finally:
        store.close()
        engine.dispose()


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_postgres_revoked_schema_and_caught_join_failure_are_atomic():
    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    schema = MetaData()
    suffix = uuid4().hex
    left = Table(f"storage_left_{suffix}", schema, Column("id", Integer, primary_key=True))
    right = Table(f"storage_right_{suffix}", schema, Column("id", Integer, primary_key=True))
    pending = Table(
        f"storage_pending_{suffix}", MetaData(), Column("id", Integer, primary_key=True)
    )
    a, b, ddl = Storage(engine, (left,)), Storage(engine, (right,)), Storage(engine, (pending,))
    event.listen(pending, "after_create", lambda *_args, **_kwargs: ddl.close())
    try:
        with pytest.raises(ValueError, match="closed"):
            initialize_stores(engine, {"fixture": {"storage": ddl}})
        assert pending.name not in inspect(engine).get_table_names()
        a.initialize(left)
        b.initialize(right)
        with pytest.raises(ValueError, match="closed"), a.begin() as root:
            root.execute(left.insert().values(id=1))
            with pytest.raises(RuntimeError), b.join(root) as participant:
                participant.execute(right.insert().values(id=2))
                raise RuntimeError("failed after database write")
        with engine.connect() as connection:
            assert connection.execute(select(left)).first() is None
            assert connection.execute(select(right)).first() is None
    finally:
        a.close()
        b.close()
        ddl.close()
        schema.drop_all(engine)
        pending.drop(engine, checkfirst=True)
        engine.dispose()
