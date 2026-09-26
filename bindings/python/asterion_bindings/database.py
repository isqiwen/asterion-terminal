"""SQLAlchemy compilation and reflection adapters for the fixed Rust database driver."""

import json
import re
from typing import ClassVar, cast
from weakref import WeakKeyDictionary

from sqlalchemy import JSON, BigInteger, Boolean, Float, Integer, String, event, exc
from sqlalchemy import create_engine as sqlalchemy_engine
from sqlalchemy.dialects import registry
from sqlalchemy.dialects.postgresql import JSON as PGJSON
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.dialects.postgresql.base import PGDialect, PGExecutionContext
from sqlalchemy.dialects.sqlite.base import SQLiteDialect, SQLiteExecutionContext
from sqlalchemy.engine import Engine, make_url, reflection
from sqlalchemy.engine.default import DefaultDialect
from sqlalchemy.pool import NullPool

from . import _database, _native
from ._call import invoke

_pools: WeakKeyDictionary[Engine, _native.NativeDatabase] = WeakKeyDictionary()


def _physical(connection):
    return connection.connection.dbapi_connection


def kernel_call(connection, action, *args):
    """Preserve driver exception types for fixed operations in this transaction."""
    try:
        return _database.native(action, *args)
    except _database.Error as error:
        raise exc.DBAPIError.instance(
            None, None, error, _database.Error, hide_parameters=True, dialect=connection.dialect
        ) from None


def database_identity(engine):
    """Trusted host bridge to the fixed native pool, never a feature plugin port."""
    return _pools[engine].identity()


def native_connection(connection):
    """Borrow the existing physical lease; this never checks out another one."""
    return _physical(connection).handle


def _column_type(kind):
    types = {
        "INTEGER": Integer,
        "INT": Integer,
        "BIGINT": BigInteger,
        "FLOAT": Float,
        "DOUBLE PRECISION": Float,
        "REAL": Float,
        "BOOLEAN": Boolean,
        "JSON": JSON,
        "JSONB": JSONB,
        "TEXT": String,
        "VARCHAR": String,
        "CHARACTER VARYING": String,
    }
    if kind in types:
        return types[kind]()
    match = re.fullmatch(r"(?:VARCHAR|CHARACTER VARYING)\((\d+)\)", kind)
    if match:
        return String(int(match[1]))
    raise ValueError("Database column type is outside the current contract")


class NativeDialect:
    name: str
    default_schema_name: str | None
    driver = "asterion"
    supports_statement_cache = True
    supports_server_side_cursors = True
    supports_sane_rowcount = True
    supports_sane_multi_rowcount = True
    # Bulk SQLAlchemy reflection must compose our native schema observation;
    # PGDialect's SQL introspection has a separate, much wider type contract.
    get_multi_columns = DefaultDialect.get_multi_columns
    get_multi_pk_constraint = DefaultDialect.get_multi_pk_constraint
    get_multi_foreign_keys = DefaultDialect.get_multi_foreign_keys
    get_multi_indexes = DefaultDialect.get_multi_indexes
    get_multi_unique_constraints = DefaultDialect.get_multi_unique_constraints
    get_multi_check_constraints = DefaultDialect.get_multi_check_constraints
    get_multi_table_comment = DefaultDialect.get_multi_table_comment
    get_multi_table_options = DefaultDialect.get_multi_table_options

    def get_check_constraints(self, connection, table_name, schema=None, **kw):
        raise NotImplementedError("CHECK reflection is outside the current storage contract")

    def get_table_comment(self, connection, table_name, schema=None, **kw):
        raise NotImplementedError("Table comments are outside the current storage contract")

    def get_table_options(self, connection, table_name, schema=None, **kw):
        raise NotImplementedError("Table options are outside the current storage contract")

    @classmethod
    def import_dbapi(cls):
        _database.sqlite_version_info = _native.database_sqlite_version()
        return _database

    def create_connect_args(self, url):
        return [], {}

    def do_begin(self, dbapi_connection):
        dbapi_connection.begin()

    def do_execute(self, cursor, statement, parameters, context=None):
        cursor.storage = context.execution_options.get("_asterion_storage") if context else None
        cursor.execute(statement, parameters)

    def do_executemany(self, cursor, statement, parameters, context=None):
        cursor.storage = context.execution_options.get("_asterion_storage") if context else None
        cursor.executemany(statement, parameters)

    def do_ping(self, dbapi_connection):
        cursor = dbapi_connection.cursor()
        try:
            cursor.execute("SELECT 1")
            return True
        finally:
            cursor.close()
            dbapi_connection.rollback()

    def is_disconnect(self, error, connection, cursor):
        return getattr(error, "code", None) == "closed"

    def get_default_isolation_level(self, dbapi_connection):
        return "SERIALIZABLE" if self.name == "sqlite" else "READ COMMITTED"

    def get_isolation_level_values(self, dbapi_connection):
        return ("AUTOCOMMIT", self.get_default_isolation_level(dbapi_connection))

    def set_isolation_level(self, connection, level):
        if level == "AUTOCOMMIT":
            connection.autocommit = True
        elif level == self.get_default_isolation_level(connection):
            connection.autocommit = False
        else:
            raise ValueError("Unsupported database isolation mode")

    @reflection.cache
    def _schema(self, connection, schema=None, **kw):
        if schema not in {None, self.default_schema_name}:
            raise ValueError("Schema selection must be configured on the database connection")
        return json.loads(_database.native(_physical(connection).handle.observe_schema))

    def _table(self, connection, name, schema=None, **kw):
        value = next(
            (table for table in self._schema(connection, schema, **kw) if table["name"] == name),
            None,
        )
        if value is None:
            raise exc.NoSuchTableError(name)
        return value

    def get_table_names(self, connection, schema=None, **kw):
        return [item["name"] for item in self._schema(connection, schema, **kw)]

    def has_table(self, connection, table_name, schema=None, **kw):
        return table_name in self.get_table_names(connection, schema, **kw)

    def get_columns(self, connection, table_name, schema=None, **kw):
        return [
            {
                "name": column["name"],
                "type": _column_type(column["kind"]),
                "nullable": column["nullable"],
                "default": None,
            }
            for column in self._table(connection, table_name, schema, **kw)["columns"]
        ]

    def get_pk_constraint(self, connection, table_name, schema=None, **kw):
        return {
            "name": None,
            "constrained_columns": self._table(connection, table_name, schema, **kw)["primary"],
        }

    def get_unique_constraints(self, connection, table_name, schema=None, **kw):
        return [
            {"name": None, "column_names": columns}
            for columns in self._table(connection, table_name, schema, **kw)["unique"]
        ]

    def get_indexes(self, connection, table_name, schema=None, **kw):
        return [
            {"name": index["name"], "column_names": index["columns"], "unique": index["unique"]}
            for index in self._table(connection, table_name, schema, **kw)["indexes"]
        ]

    def get_foreign_keys(self, connection, table_name, schema=None, **kw):
        return [
            {
                "name": None,
                "constrained_columns": foreign["columns"],
                "referred_schema": None,
                "referred_table": foreign["table"],
                "referred_columns": foreign["references"],
                "options": {},
            }
            for foreign in self._table(connection, table_name, schema, **kw)["foreign"]
        ]


class SQLiteContext(SQLiteExecutionContext):
    def create_server_side_cursor(self):
        return self._dbapi_connection.cursor(stream=True)


class PostgresContext(PGExecutionContext):
    def get_result_processor(self, type_, colname, coltype):
        processor = super().get_result_processor(type_, colname, coltype)
        if processor is None and coltype in {"json", "jsonb"}:
            # Textual SQL has no SQLAlchemy type information. Native PG cursor
            # metadata still identifies JSON; never infer it from string content.
            deserialize = cast(PGDialect, self.dialect)._json_deserializer or json.loads
            return lambda value: None if value is None else deserialize(value)
        return processor

    def create_server_side_cursor(self):
        return self._dbapi_connection.cursor(stream=True)


class SQLiteNative(NativeDialect, SQLiteDialect):
    supports_statement_cache = True
    execution_ctx_cls = SQLiteContext
    default_paramstyle = "qmark"

    def _get_server_version_info(self, connection):
        return _native.database_sqlite_version()


class NativeJSON(PGJSON):
    # The native wire contract returns serialized JSON, including JSON scalars.
    result_processor = JSON.result_processor


class NativeJSONB(JSONB):
    result_processor = JSON.result_processor


class PostgresNative(NativeDialect, PGDialect):
    colspecs: ClassVar = {
        **PGDialect.colspecs,
        JSON: NativeJSON,
        PGJSON: NativeJSON,
        JSONB: NativeJSONB,
    }
    supports_statement_cache = True
    execution_ctx_cls = PostgresContext
    default_paramstyle = "numeric_dollar"

    def __init__(self, **kwargs):
        super().__init__(paramstyle="numeric_dollar", **kwargs)


registry.register("sqlite.asterion", __name__, "SQLiteNative")
registry.register("postgresql.asterion", __name__, "PostgresNative")


def create_engine(url, *, connect_args=None, pool_size=5, pool_timeout=30, **options):
    """Create one native pool; SQLAlchemy owns compilation and result conversion only."""
    text = url if isinstance(url, str) else url.render_as_string(hide_password=False)
    # The kernel owns URL interpretation; SQLAlchemy only selects the dialect.
    config = invoke("kernel", "database.config", {"url": text})
    url = make_url(text)
    backend = config["backend"]
    arguments = dict(connect_args or {})
    if backend == "sqlite":
        config["timeout_ms"] = round(arguments.pop("timeout", 5) * 1000)
        if config["path"] == ":memory:":
            pool_size = 1
    else:
        config["connect_timeout_ms"] = round(arguments.pop("connect_timeout", 5) * 1000)
        config["options"] = arguments.pop("options", "")
    if arguments:
        raise ValueError("Unsupported database connection options")
    pool = _database.native(_native.NativeDatabase, json.dumps(config), pool_size, pool_timeout)
    options.setdefault("hide_parameters", True)
    engine = sqlalchemy_engine(
        url.set(drivername=f"{backend}+asterion"),
        creator=lambda: _database.Connection(_database.native(pool.connect)),
        poolclass=NullPool,
        **options,
    )
    _pools[engine] = pool
    event.listen(engine, "engine_disposed", lambda _: _database.native(pool.dispose))
    return engine
