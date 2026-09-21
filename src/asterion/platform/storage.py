"""Scoped relational storage for trusted plugins, with core-owned transactions and DDL.

This is a cooperative in-process boundary, not a SQL or operating-system sandbox.
"""

from contextlib import contextmanager
from dataclasses import dataclass

from sqlalchemy import Table, UniqueConstraint, inspect, select
from sqlalchemy.sql import visitors
from sqlalchemy.sql.ddl import sort_tables
from sqlalchemy.sql.dml import UpdateBase
from sqlalchemy.sql.elements import TextClause
from sqlalchemy.sql.selectable import SelectBase, TableClause


@dataclass(frozen=True)
class Dialect:
    name: str


class Transaction:
    def __init__(self, connection, readable, writable, owner=None, parent=None):
        self._connection = connection
        self._owner, self._parent = owner, parent
        self._readable, self._writable = readable, writable
        self.dialect = Dialect(connection.dialect.name)
        self._active = True

    def execute(self, statement, parameters=None):
        connection = transaction_connection(self)
        nodes = tuple(visitors.iterate(statement))
        if any(isinstance(node, TextClause) for node in nodes):
            raise ValueError("Raw SQL is not part of the storage contract")
        if any(isinstance(node, TableClause) and not isinstance(node, Table) for node in nodes):
            raise ValueError("Only declared table objects are supported")
        if any(isinstance(node, UpdateBase) and node.table not in self._writable for node in nodes):
            raise ValueError("Storage write is outside the granted tables")
        tables = {node for node in nodes if isinstance(node, Table)}
        if not tables <= self._readable:
            raise ValueError("Storage read is outside the granted tables")
        if not isinstance(statement, SelectBase):
            if not (
                getattr(statement, "is_insert", False)
                or getattr(statement, "is_update", False)
                or getattr(statement, "is_delete", False)
            ):
                raise ValueError("Only structured queries and row mutations are supported")
            if statement.table not in self._writable:
                raise ValueError("Storage write is outside the granted tables")
        if self._owner and self._owner._filters:
            replacements = {}
            for table, condition in self._owner._filters.items():
                replacements[table] = select(table).where(condition).subquery()

            def replace(element, **kw):
                node = element
                if isinstance(node, Table) and node in replacements:
                    return replacements[node]
                table = getattr(node, "table", None)
                if table in replacements and getattr(node, "name", None) in replacements[table].c:
                    return replacements[table].c[node.name]
                return None

            statement = visitors.replacement_traverse(statement, {}, replace)
        return (
            connection.execute(statement, parameters)
            if parameters is not None
            else connection.execute(statement)
        )

    def close(self):
        self._active = False
        self._connection = None


def transaction_connection(transaction):
    """Core-only bridge for atomic task operations and explicitly bound domain readers."""
    if (
        not isinstance(transaction, Transaction)
        or not transaction._active
        or transaction._connection is None
    ):
        raise ValueError("Storage transaction is closed or invalid")
    if transaction._owner:
        transaction._owner._require_active()
    if transaction._parent:
        transaction_connection(transaction._parent)
    return transaction._connection


class Storage:
    def __init__(self, engine, tables, *, read=(), filters=None):
        self._engine = engine
        self._writable = frozenset(tables)
        self._readable = self._writable | frozenset(read)
        self._active = True
        self._filters = dict(filters or {})
        self.dialect = Dialect(engine.dialect.name)

    def _require_active(self):
        if not self._active:
            raise ValueError("Storage is closed")

    def initialize(self, *tables):
        self._require_active()
        if not set(tables) <= self._writable:
            raise ValueError("Cannot initialize unowned tables")
        initialize_schema(self._engine, tables)

    @contextmanager
    def _open(self, write):
        self._require_active()
        with self._engine.begin() if write else self._engine.connect() as connection:
            transaction = Transaction(
                connection, self._readable, self._writable if write else frozenset(), self
            )
            try:
                yield transaction
                self._require_active()
            finally:
                transaction.close()

    def begin(self):
        return self._open(True)

    def connect(self):
        return self._open(False)

    @contextmanager
    def borrow(self, transaction):
        self._require_active()
        connection = transaction_connection(transaction)
        if connection.engine is not self._engine:
            raise ValueError("Cannot borrow another database transaction")
        reader = Transaction(connection, self._readable, frozenset(), self, transaction)
        try:
            yield reader
        finally:
            reader.close()

    @contextmanager
    def join(self, transaction):
        """Join a writable transaction with only this storage owner's declared grants."""
        self._require_active()
        connection = transaction_connection(transaction)
        if connection.engine is not self._engine:
            raise ValueError("Cannot join another database transaction")
        if not transaction._writable:
            raise ValueError("Cannot join a read-only transaction for writing")
        writer = Transaction(connection, self._readable, self._writable, self, transaction)
        try:
            yield writer
            self._require_active()
        finally:
            writer.close()

    def close(self):
        self._active = False


def initialize_schema(engine, tables):
    """Preflight all existing tables before creating missing current-contract tables."""
    tables = tuple(tables)
    with engine.begin() as connection:
        inspector = inspect(connection)
        existing = set(inspector.get_table_names())
        for table in tables:
            if table.name not in existing:
                continue
            actual = {column["name"]: column for column in inspector.get_columns(table.name)}
            if set(actual) != set(table.c.keys()):
                raise ValueError(
                    f"Database structure does not match current contract: {table.name}"
                )
            for column in table.c:
                found = actual[column.name]
                expected_type = column.type.compile(dialect=connection.dialect)
                actual_type = found["type"].compile(dialect=connection.dialect)
                if (
                    expected_type != actual_type
                    and not (expected_type == "FLOAT" and actual_type == "DOUBLE PRECISION")
                ) or found["nullable"] != column.nullable:
                    raise ValueError(
                        f"Database column does not match current contract: {table.name}.{column.name}"
                    )
            primary = inspector.get_pk_constraint(table.name)["constrained_columns"]
            if set(primary) != set(table.primary_key.columns.keys()):
                raise ValueError(f"Database key does not match current contract: {table.name}")
            unique = {
                tuple(sorted(item["column_names"]))
                for item in inspector.get_unique_constraints(table.name)
            }
            unique |= {
                tuple(sorted(item["column_names"]))
                for item in inspector.get_indexes(table.name)
                if item["unique"]
            }
            expected_unique = {
                tuple(sorted(constraint.columns.keys()))
                for constraint in table.constraints
                if isinstance(constraint, UniqueConstraint)
            }
            expected_unique |= {
                tuple(sorted(index.columns.keys())) for index in table.indexes if index.unique
            }
            if not expected_unique <= unique:
                raise ValueError(
                    f"Database uniqueness does not match current contract: {table.name}"
                )
            actual_foreign = {
                (
                    tuple(item["constrained_columns"]),
                    item["referred_table"],
                    tuple(item["referred_columns"]),
                )
                for item in inspector.get_foreign_keys(table.name)
            }
            expected_foreign = {
                (
                    tuple(element.parent.name for element in constraint.elements),
                    constraint.referred_table.name,
                    tuple(element.column.name for element in constraint.elements),
                )
                for constraint in table.foreign_key_constraints
            }
            if expected_foreign != actual_foreign:
                raise ValueError(f"Database references do not match current contract: {table.name}")
        for table in sort_tables(tables):
            if table.name not in existing:
                table.create(connection)


def initialize_stores(engine, resources, core_tables=()):
    owners = {}
    tables = list(core_tables)
    for identifier, grants in resources.items():
        for value in grants.values():
            if not isinstance(value, Storage):
                continue
            if value._engine is not engine:
                raise ValueError("Storage belongs to another database")
            for table in value._writable:
                if table in owners or table in core_tables:
                    raise ValueError(f"Duplicate storage ownership: {table.name}")
                owners[table] = identifier
                tables.append(table)
    initialize_schema(engine, tables)
