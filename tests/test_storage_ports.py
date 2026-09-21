import pytest
from sqlalchemy import (
    Column,
    Integer,
    MetaData,
    String,
    Table,
    create_engine,
    inspect,
    select,
    text,
)

from asterion.platform.storage import Storage, initialize_schema, initialize_stores
from asterion.platform.store import jobs
from asterion.platform.task_port import task_port
from asterion.platform.tasks.service import Tasks


@pytest.fixture
def stores():
    engine = create_engine("sqlite://")
    metadata = MetaData()
    owned = Table("owned", metadata, Column("id", Integer, primary_key=True))
    foreign = Table("foreign", metadata, Column("id", Integer, primary_key=True))
    initialize_schema(engine, (owned, foreign, jobs))
    store = Storage(engine, (owned,), read=(jobs,))
    yield engine, store, owned, foreign
    engine.dispose()


def test_storage_denies_foreign_tables_raw_sql_and_core_writes(stores):
    _, store, owned, foreign = stores
    with store.begin() as conn:
        conn.execute(owned.insert().values(id=1))
        for statement in (
            select(foreign),
            foreign.insert().values(id=1),
            jobs.update().values(state="SUCCEEDED"),
            text("DELETE FROM owned"),
            select(owned).add_cte(foreign.delete().cte()),
        ):
            with pytest.raises(ValueError):
                conn.execute(statement)
    with store.connect() as conn:
        assert conn.execute(select(owned.c.id)).scalar_one() == 1
        with pytest.raises(ValueError):
            conn.execute(owned.delete())
    with pytest.raises(ValueError, match="closed"):
        conn.execute(select(owned))
    store.close()
    with pytest.raises(ValueError, match="closed"), store.begin():
        pass


def test_domain_and_core_task_completion_commit_or_rollback_together(stores):
    engine, store, owned, _ = stores
    tasks = Tasks(engine)
    port = task_port(tasks, frozenset({"fixture.run"}))
    job = tasks.submit("run", "fixture.run", {})
    claimed = tasks.claim("worker")
    with pytest.raises(RuntimeError), store.begin() as conn:
        conn.execute(owned.insert().values(id=1))
        port.complete(conn, job["id"], claimed["token"], {"id": 1})
        raise RuntimeError("publication failed")
    assert tasks.get(job["id"])["state"] == "RUNNING"
    with store.connect() as conn:
        assert conn.execute(select(owned)).first() is None
    with store.begin() as conn:
        conn.execute(owned.insert().values(id=1))
        port.complete(conn, job["id"], claimed["token"], {"id": 1})
    assert tasks.get(job["id"])["state"] == "SUCCEEDED"


def test_borrowed_domain_reader_keeps_transaction_and_expires(stores):
    engine, store, owned, foreign = stores
    reader = Storage(engine, (foreign,))
    with store.begin() as conn:
        with reader.borrow(conn) as borrowed:
            assert borrowed.execute(select(foreign)).first() is None
            with pytest.raises(ValueError):
                borrowed.execute(select(owned))
            with pytest.raises(ValueError):
                borrowed.execute(foreign.insert().values(id=1))
        with pytest.raises(ValueError, match="closed"):
            borrowed.execute(select(foreign))


@pytest.mark.parametrize("damage", ["missing_column", "type", "nullable", "key"])
def test_schema_preflight_rejects_current_contract_damage_before_creating_tables(damage):
    engine = create_engine("sqlite://")
    metadata = MetaData()
    current = Table(
        "records",
        metadata,
        Column("id", Integer, primary_key=True),
        Column("name", String, nullable=False),
    )
    pending = Table("pending", metadata, Column("id", Integer, primary_key=True))
    ddl = {
        "missing_column": "CREATE TABLE records (id INTEGER NOT NULL PRIMARY KEY)",
        "type": "CREATE TABLE records (id INTEGER NOT NULL PRIMARY KEY, name INTEGER NOT NULL)",
        "nullable": "CREATE TABLE records (id INTEGER NOT NULL PRIMARY KEY, name VARCHAR)",
        "key": "CREATE TABLE records (id INTEGER NOT NULL, name VARCHAR NOT NULL)",
    }[damage]
    try:
        with engine.begin() as conn:
            conn.execute(text(ddl))
        with pytest.raises(ValueError, match="contract"):
            initialize_schema(engine, (pending, current))
        assert set(inspect(engine).get_table_names()) == {"records"}
    finally:
        engine.dispose()


def test_two_plugins_cannot_own_the_same_table(stores):
    engine, store, _, _ = stores
    with pytest.raises(ValueError, match="ownership"):
        initialize_stores(engine, {"one": {"storage": store}, "two": {"storage": store}})


def test_filtered_task_reads_cannot_cross_domain_even_with_alias_or_subquery(stores):
    engine, _, _, _ = stores
    from asterion.distribution_storage import data_storage, research_storage

    tasks = Tasks(engine)
    data = tasks.submit("data", "data.sync", {"private": "data"})
    research = tasks.submit("research", "research.backtest", {"private": "research"})
    for store, expected in [
        (data_storage(engine), data["id"]),
        (research_storage(engine), research["id"]),
    ]:
        with store.connect() as conn:
            for table in (jobs, jobs.alias(), select(jobs).subquery()):
                assert list(conn.execute(select(table.c.id)).scalars()) == [expected]


def test_closing_storage_revokes_open_transaction_and_rolls_back(stores):
    engine, store, owned, _ = stores
    with pytest.raises(ValueError, match="closed"), store.begin() as conn:
        conn.execute(owned.insert().values(id=1))
        store.close()
    with engine.connect() as conn:
        assert conn.execute(select(owned)).first() is None
