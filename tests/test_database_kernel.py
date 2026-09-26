"""Real native driver and SQLAlchemy adapter behavior, never the retired DBAPI."""

import json
import os

import pytest
from asterion_bindings import _database
from asterion_bindings.database import create_engine
from sqlalchemy import (
    JSON,
    BigInteger,
    Boolean,
    Column,
    Float,
    Integer,
    MetaData,
    String,
    Table,
    inspect,
    select,
    text,
)
from sqlalchemy.exc import DBAPIError, IntegrityError


def test_native_driver_types_batch_writes_and_atomic_rollback(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/native.db")
    metadata = MetaData()
    table = Table(
        "native_values",
        metadata,
        Column("id", Integer, primary_key=True),
        Column("large", BigInteger),
        Column("flag", Boolean),
        Column("fraction", Float),
        Column("label", String),
        Column("payload", JSON),
    )
    try:
        metadata.create_all(engine)
        rows = [
            {
                "id": 1,
                "large": 2**63 - 1,
                "flag": True,
                "fraction": 0.125,
                "label": "夜盘",
                "payload": {"null": None, "decimal": "0.1000"},
            },
            {
                "id": 2,
                "large": -(2**63),
                "flag": False,
                "fraction": None,
                "label": None,
                "payload": [1, False, None],
            },
        ]
        with engine.begin() as conn:
            assert isinstance(conn.connection.dbapi_connection, _database.Connection)
            assert conn.execute(table.insert(), rows).rowcount == 2
        with engine.connect() as conn:
            assert [
                dict(row) for row in conn.execute(select(table).order_by(table.c.id)).mappings()
            ] == rows
        with pytest.raises(IntegrityError), engine.begin() as conn:
            conn.execute(table.insert().values(id=3))
            conn.execute(table.insert().values(id=1))
        with engine.connect() as conn:
            assert conn.execute(select(table.c.id).order_by(table.c.id)).scalars().all() == [1, 2]
    finally:
        engine.dispose()


def test_memory_pool_persists_per_engine_but_never_shares_engines():
    left, right = create_engine("sqlite://"), create_engine("sqlite://")
    try:
        with left.begin() as conn:
            conn.execute(text("CREATE TABLE independent (id INTEGER NOT NULL PRIMARY KEY)"))
            conn.execute(text("INSERT INTO independent VALUES (1)"))
        with left.connect() as conn:
            assert conn.execute(text("SELECT id FROM independent")).scalar_one() == 1
        assert "independent" not in inspect(right).get_table_names()
    finally:
        left.dispose()
        right.dispose()


def test_caught_batch_failure_blocks_commit_and_pool_recovers():
    engine = create_engine("sqlite://")
    try:
        with engine.begin() as conn:
            conn.execute(text("CREATE TABLE atomic_batch (id INTEGER NOT NULL PRIMARY KEY)"))
        with (
            pytest.raises(DBAPIError, match="rollback"),
            engine.begin() as conn,
            pytest.raises(IntegrityError),
        ):
            conn.execute(text("INSERT INTO atomic_batch VALUES (:id)"), [{"id": 1}, {"id": 1}])
        with engine.connect() as conn:
            assert conn.execute(text("SELECT COUNT(*) FROM atomic_batch")).scalar_one() == 0
        with engine.begin() as conn:
            conn.execute(text("INSERT INTO atomic_batch VALUES (2)"))
        with engine.connect() as conn:
            assert conn.execute(text("SELECT id FROM atomic_batch")).scalar_one() == 2
    finally:
        engine.dispose()


def test_streaming_cursor_is_bounded_exclusive_and_releases_its_connection():
    engine = create_engine("sqlite://")
    try:
        with engine.connect() as conn:
            result = conn.execute(
                text(
                    "WITH RECURSIVE items(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM items WHERE n<5000) SELECT n FROM items"
                ).execution_options(yield_per=1000)
            )
            assert len(result.fetchmany(1500)) == 1500
            with pytest.raises(DBAPIError):
                conn.execute(text("SELECT 1"))
            result.close()
            assert conn.execute(text("SELECT 1")).scalar_one() == 1
        with engine.connect() as conn:
            assert conn.execute(text("SELECT 2")).scalar_one() == 2
    finally:
        engine.dispose()


def test_native_pool_exhaustion_is_bounded_and_returned_cursor_is_invalid():
    engine = create_engine("sqlite://", pool_timeout=0.02)
    try:
        with engine.connect() as conn:
            cursor = conn.connection.dbapi_connection.cursor()
            cursor.execute("SELECT 1")
            with pytest.raises(DBAPIError):
                engine.connect()
        with pytest.raises(_database.Error):
            cursor.fetchone()
        with engine.connect() as conn:
            assert conn.execute(text("SELECT 2")).scalar_one() == 2
    finally:
        engine.dispose()


@pytest.mark.skipif(
    not os.environ.get("ASTERION_TEST_DATABASE_URL"), reason="Isolated PostgreSQL URL required"
)
def test_postgres_native_cursor_json_and_readonly_transaction():
    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    try:
        with engine.connect() as conn:
            assert isinstance(conn.connection.dbapi_connection, _database.Connection)
            assert (
                conn.execute(
                    text("SELECT CAST(:number AS BIGINT)"), {"number": 2**63 - 1}
                ).scalar_one()
                == 2**63 - 1
            )
            conn.rollback()
            conn.execute(text("SET TRANSACTION READ ONLY"))
            result = conn.execute(
                text("SELECT generate_series(1,100001) AS n").execution_options(yield_per=1000)
            )
            rows = 0
            for row in result:
                rows += 1
                assert row.n == rows
            assert rows == 100001
    finally:
        engine.dispose()


def test_factory_accepts_its_current_native_engine_url(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/clone.db")
    clone = create_engine(engine.url)
    try:
        with clone.connect() as conn:
            assert conn.execute(text("SELECT 1")).scalar_one() == 1
    finally:
        clone.dispose()
        engine.dispose()


@pytest.mark.skipif(
    not os.environ.get("ASTERION_TEST_DATABASE_URL"), reason="Isolated PostgreSQL URL required"
)
def test_postgres_typed_json_objects_and_scalars_roundtrip():
    from uuid import uuid4

    from sqlalchemy.dialects.postgresql import JSONB

    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    table = Table(
        "native_json_" + uuid4().hex,
        MetaData(),
        Column("id", Integer, primary_key=True),
        Column("payload", JSON),
        Column("binary", JSONB),
        Column("text", String),
    )
    values = [
        {"中": [None, True, 1, {"nested": "value"}]},
        [False, 2.5],
        "literal",
        '{"still": "a string"}',
        42,
        0.125,
        True,
        False,
        None,
    ]
    try:
        with engine.begin() as conn:
            table.create(conn)
            for index, value in enumerate(values):
                assert (
                    conn.execute(
                        table.insert()
                        .values(id=index, payload=value, binary=value, text='{"plain": "text"}')
                        .returning(table.c.payload)
                    ).scalar_one()
                    == value
                )
            result = conn.execute(select(table).order_by(table.c.id)).mappings().all()
            assert [row["payload"] for row in result] == values
            assert [row["binary"] for row in result] == values
            assert all(row["text"] == '{"plain": "text"}' for row in result)
        with engine.connect() as conn:
            assert (
                conn.execute(select(table.c.payload).order_by(table.c.id)).scalars().all() == values
            )
            reflected = Table(table.name, MetaData(), autoload_with=conn)
            for column in (reflected.c.payload, reflected.c.binary):
                assert (
                    conn.execute(select(column).order_by(reflected.c.id)).scalars().all() == values
                )
            statement = text(f'SELECT "payload", "binary", "text" FROM "{table.name}" ORDER BY id')
            for query in (statement, statement.execution_options(yield_per=2)):
                result = conn.execute(query).all()
                assert [row.payload for row in result] == values
                assert [row.binary for row in result] == values
                assert all(row.text == '{"plain": "text"}' for row in result)
            assert conn.execute(text("SELECT NULL::JSON, NULL::JSONB, 'null'::TEXT")).one() == (
                None,
                None,
                "null",
            )
    finally:
        with engine.begin() as conn:
            table.drop(conn, checkfirst=True)
        engine.dispose()


@pytest.mark.parametrize(
    "statement", ["COMMIT", "/* boundary */ ROLLBACK", ";-- comment\nEND", "SAVEPOINT named"]
)
def test_sql_transaction_control_cannot_bypass_native_epoch(statement):
    from sqlalchemy.exc import ProgrammingError

    engine = create_engine("sqlite://")
    try:
        with engine.begin() as conn:
            conn.execute(text("CREATE TABLE controlled (id INTEGER PRIMARY KEY)"))
        with pytest.raises(RuntimeError), engine.begin() as conn:
            conn.execute(text("INSERT INTO controlled VALUES (1)"))
            with pytest.raises(ProgrammingError):
                conn.execute(text(statement))
            raise RuntimeError("roll back the original physical transaction")
        with engine.connect() as conn:
            assert conn.execute(text("SELECT COUNT(*) FROM controlled")).scalar_one() == 0
    finally:
        engine.dispose()


@pytest.mark.skipif(
    not os.environ.get("ASTERION_TEST_DATABASE_URL"), reason="Isolated PostgreSQL URL required"
)
def test_postgres_backup_evidence_receives_typed_json_from_textual_query():
    from asterion.contract_rules.backup import load_evidence

    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    spec = {"fixture": [None, True, 1, {"text": "中文"}]}
    try:
        with engine.begin() as conn:
            conn.execute(
                text(
                    "CREATE TEMP TABLE contract_rule_versions(id TEXT PRIMARY KEY, spec JSON NOT NULL)"
                )
            )
            conn.execute(
                text("INSERT INTO contract_rule_versions VALUES (:id, CAST(:spec AS JSON))"),
                {"id": "fixture", "spec": json.dumps(spec)},
            )
            assert list(load_evidence(conn).versions()) == [{"id": "fixture", "spec": spec}]
    finally:
        engine.dispose()
