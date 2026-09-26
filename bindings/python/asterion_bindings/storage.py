"""SQLAlchemy statement/driver adaptation for fixed Rust storage grants.

The adapter preserves one driver transaction across business, task and event
writes. Rust owns grant checks, handle lifetime, revocation and schema preflight.
SQLAlchemy is a trusted host statement compiler, not an isolation boundary for
arbitrary Python code. Untrusted plugins cannot obtain this in-process adapter.
"""

import json
from contextlib import contextmanager
from dataclasses import dataclass

from sqlalchemy import Table, UniqueConstraint, inspect, select
from sqlalchemy.sql import visitors
from sqlalchemy.sql.ddl import sort_tables
from sqlalchemy.sql.dml import UpdateBase
from sqlalchemy.sql.elements import ColumnClause, TextClause
from sqlalchemy.sql.functions import count
from sqlalchemy.sql.selectable import SelectBase, TableClause

from . import _native
from .database import database_identity, kernel_call, native_connection


@dataclass(frozen=True)
class Dialect:
    name: str


class _Result:
    """Guard consumption, including SQLAlchemy's already prefetched row buffers.

    SQLAlchemy still owns conversion, cardinality and view behavior. This facade
    exposes the current storage result contract; it never exports raw cursors or
    permits results from unrelated owners to be merged.
    """

    _views = frozenset({"mappings", "scalars", "columns", "unique", "yield_per", "tuples"})
    _reads = frozenset(
        {
            "all",
            "fetchall",
            "fetchone",
            "fetchmany",
            "first",
            "one",
            "one_or_none",
            "scalar",
            "scalar_one",
            "scalar_one_or_none",
            "keys",
        }
    )
    _metadata = frozenset(
        {
            "rowcount",
            "lastrowid",
            "inserted_primary_key",
            "inserted_primary_key_rows",
            "returned_defaults",
            "returned_defaults_rows",
            "returns_rows",
            "is_insert",
        }
    )

    def __init__(self, result, handle):
        self._result, self._handle = result, handle

    def _failed(self):
        self._handle.abort()
        self.close()

    def _use(self, action, *args, **kwargs):
        try:
            self._handle.check_bound()
            try:
                value = action(*args, **kwargs)
            except StopIteration:
                self._handle.check_bound()
                raise
            self._handle.check_bound()
            return value
        except StopIteration:
            raise
        except BaseException:
            self._failed()
            raise

    def __getattr__(self, name):
        if name in self._metadata:
            return self._use(getattr, self._result, name)
        if name not in self._views | self._reads:
            raise AttributeError(f"Storage result does not expose {name}")

        def call(*args, **kwargs):
            value = self._use(getattr(self._result, name), *args, **kwargs)
            return _Result(value, self._handle) if name in self._views else value

        return call

    @property
    def t(self):
        return _Result(self._use(getattr, self._result, "t"), self._handle)

    @property
    def closed(self):
        return self._result.closed

    def __iter__(self):
        return self

    def __next__(self):
        return self._use(next, self._result)

    def partitions(self, size=None):
        return _Partitions(self, self._use(self._result.partitions, size))

    def close(self):
        self._result.close()

    def __enter__(self):
        self._use(lambda: None)
        return self

    def __exit__(self, *exception):
        self.close()


class _Partitions:
    def __init__(self, result, iterator):
        self._result, self._iterator = result, iterator

    def __iter__(self):
        return self

    def __next__(self):
        return self._result._use(next, self._iterator)

    def close(self):
        self._iterator.close()
        self._result.close()


class Transaction:
    def __init__(self, connection, handle, filters):
        self._connection = connection
        self._handle = handle
        self._filters = filters
        self.dialect = Dialect(connection.dialect.name)

    @property
    def writable(self):
        return self._handle.writable()

    def execute(self, statement, parameters=None):
        try:
            connection = transaction_connection(self)
        except BaseException:
            self._handle.abort()
            raise
        nodes = tuple(visitors.iterate(statement))
        # count() emits one literal '*' internally. All other textual columns
        # would bypass the declared-table walk (including scalar SQL subqueries).
        count_stars = {
            id(argument)
            for node in nodes
            if isinstance(node, count)
            for argument in node.clauses
            if isinstance(argument, ColumnClause) and argument.is_literal and argument.name == "*"
        }
        operation = (
            "select"
            if isinstance(statement, SelectBase)
            else next(
                (
                    name
                    for name in ("insert", "update", "delete")
                    if getattr(statement, f"is_{name}", False)
                ),
                "unsupported",
            )
        )
        self._handle.authorize(
            json.dumps(
                {
                    "operation": operation,
                    "raw": any(
                        isinstance(node, TextClause)
                        or (
                            isinstance(node, ColumnClause)
                            and node.is_literal
                            and id(node) not in count_stars
                        )
                        for node in nodes
                    ),
                    "undeclared": any(
                        isinstance(node, TableClause) and not isinstance(node, Table)
                        for node in nodes
                    ),
                    "reads": list({id(node) for node in nodes if isinstance(node, Table)}),
                    "writes": list(
                        {id(node.table) for node in nodes if isinstance(node, UpdateBase)}
                    ),
                }
            )
        )
        if self._filters:
            replacements = {
                table: select(table).where(condition).subquery()
                for table, condition in self._filters.items()
            }

            def replace(element, **kw):
                if isinstance(element, Table) and element in replacements:
                    return replacements[element]
                table = getattr(element, "table", None)
                name = getattr(element, "name", None)
                if (
                    table in replacements
                    and isinstance(name, str)
                    and name in replacements[table].c
                ):
                    return replacements[table].c[name]
                return None

            statement = visitors.replacement_traverse(statement, {}, replace)
        try:
            options = {"_asterion_storage": self._handle}
            result = (
                connection.execute(statement, parameters, execution_options=options)
                if parameters is not None
                else connection.execute(statement, execution_options=options)
            )
            self._handle.check_bound()
            return _Result(result, self._handle)
        except BaseException:
            self._handle.abort()
            raise

    def close(self):
        self._handle.close()
        self._connection = None


def transaction_connection(transaction, *, write=False):
    """Private driver bridge; scoped callers never receive a second connection."""
    if not isinstance(transaction, Transaction):
        raise TypeError("Storage transaction is closed or invalid")
    transaction._handle.check()
    if write:
        transaction._handle.require_write()
    if transaction._connection is None:
        raise ValueError("Storage transaction is closed or invalid")
    return transaction._connection


class Storage:
    def __init__(self, engine, tables, *, read=(), filters=None):
        self._engine = engine
        self._tables = tuple(tables)
        self._read = tuple(read)
        self._filters = dict(filters or {})
        self._scope = _native.StorageScope(
            database_identity(engine), list(map(id, self._tables)), list(map(id, self._read))
        )
        self.dialect = Dialect(engine.dialect.name)

    def _guarded(self, lifetime):
        result = object.__new__(type(self))
        result.__dict__ = self.__dict__.copy()
        result._scope = self._scope.guarded(lifetime)
        return result

    def initialize(self, *tables):
        self._scope.initialize(list(map(id, tables)))
        initialize_schema(self._engine, tables, participants=(self._scope,))

    @contextmanager
    def _open(self, write):
        handle = self._scope.begin(write)
        try:
            with self._engine.connect() as connection:
                physical = native_connection(connection)
                if write:
                    # Reserve SQLite's writer before reads can form an upgrade
                    # deadlock. SQLAlchemy begin then joins this physical BEGIN.
                    kernel_call(connection, physical.begin_write)
                with connection.begin() as boundary:
                    kernel_call(connection, handle.bind, physical)
                    transaction = Transaction(connection, handle, self._filters)
                    try:
                        yield transaction
                        handle.commit_ready()
                        if not write:
                            boundary.rollback()
                    finally:
                        transaction.close()
        finally:
            handle.close()

    def begin(self):
        return self._open(True)

    def connect(self):
        return self._open(False)

    @contextmanager
    def _join(self, transaction, write):
        connection = transaction_connection(transaction)
        handle = self._scope.join(transaction._handle, write)
        borrowed = Transaction(connection, handle, self._filters)
        try:
            yield borrowed
            handle.check()
        except BaseException:
            handle.abort()
            raise
        finally:
            borrowed.close()

    def borrow(self, transaction):
        return self._join(transaction, False)

    def join(self, transaction):
        return self._join(transaction, True)

    def close(self):
        self._scope.close()


def _kind(column_type, dialect):
    compiled = column_type.compile(dialect=dialect)
    # PostgreSQL's physical spelling for SQLAlchemy Float. This is dialect
    # normalization of the current type, not a historical schema conversion.
    return "FLOAT" if dialect.name == "postgresql" and compiled == "DOUBLE PRECISION" else compiled


def _declaration(table, dialect):
    unique = [
        sorted(constraint.columns.keys())
        for constraint in table.constraints
        if isinstance(constraint, UniqueConstraint)
    ]
    unique.extend(sorted(index.columns.keys()) for index in table.indexes if index.unique)
    return {
        "name": table.name,
        "columns": [
            {"name": column.name, "kind": _kind(column.type, dialect), "nullable": column.nullable}
            for column in table.c
        ],
        "primary": list(table.primary_key.columns.keys()),
        "unique": unique,
        "foreign": [
            {
                "columns": [element.parent.name for element in constraint.elements],
                "table": constraint.referred_table.name,
                "references": [element.column.name for element in constraint.elements],
            }
            for constraint in table.foreign_key_constraints
        ],
    }


def _observed(inspector, name, dialect):
    unique = [item["column_names"] for item in inspector.get_unique_constraints(name)]
    unique.extend(item["column_names"] for item in inspector.get_indexes(name) if item["unique"])
    return {
        "name": name,
        "columns": [
            {
                "name": item["name"],
                "kind": _kind(item["type"], dialect),
                "nullable": item["nullable"],
            }
            for item in inspector.get_columns(name)
        ],
        "primary": inspector.get_pk_constraint(name)["constrained_columns"],
        "unique": unique,
        "foreign": [
            {
                "columns": item["constrained_columns"],
                "table": item["referred_table"],
                "references": item["referred_columns"],
            }
            for item in inspector.get_foreign_keys(name)
        ],
    }


def initialize_schema(engine, tables, *, participants=()):
    """Observe current schema, preflight in Rust, then execute approved DDL."""
    tables = tuple(tables)
    scope = _native.StorageScope(database_identity(engine), list(map(id, tables)), [])
    handle = scope.begin(True)
    joined = []
    try:
        for participant in participants:
            joined.append(participant.join(handle, True))
        with engine.connect() as connection:
            physical = native_connection(connection)
            kernel_call(connection, physical.begin_write)
            with connection.begin():
                kernel_call(connection, handle.bind, physical)
                inspector = inspect(connection)
                existing = set(inspector.get_table_names())
                _native.storage_preflight(
                    json.dumps([_declaration(table, connection.dialect) for table in tables]),
                    json.dumps(
                        [
                            _observed(inspector, table.name, connection.dialect)
                            for table in tables
                            if table.name in existing
                        ]
                    ),
                )
                for table in sort_tables(tables):
                    if table.name not in existing:
                        table.create(connection)
                handle.commit_ready()
    finally:
        for participant in joined:
            participant.close()
        handle.close()
        scope.close()


def initialize_stores(engine, resources, core_tables=()):
    stores = [
        value
        for grants in resources.values()
        for value in grants.values()
        if isinstance(value, Storage)
    ]
    for store in stores:
        store._scope.check()
        if store._engine is not engine:
            raise ValueError("Storage belongs to another database")
    _native.storage_ownership(
        [table.name for table in core_tables],
        [[table.name for table in store._tables] for store in stores],
    )
    initialize_schema(
        engine,
        [*core_tables, *(table for store in stores for table in store._tables)],
        participants=tuple(store._scope for store in stores),
    )
