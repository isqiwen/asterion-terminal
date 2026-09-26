"""DBAPI shape only; native connection leases own all database I/O and resources."""

import json

from . import _native

apilevel = "2.0"
threadsafety = 2
paramstyle = "qmark"
sqlite_version_info: tuple[int, int, int]


class Error(Exception):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


class DatabaseError(Error): ...


class InterfaceError(Error): ...


class DataError(DatabaseError): ...


class OperationalError(DatabaseError): ...


class IntegrityError(DatabaseError): ...


class InternalError(DatabaseError): ...


class ProgrammingError(DatabaseError): ...


class NotSupportedError(DatabaseError): ...


def native(action, *args):
    try:
        return action(*args)
    except _native.NativeDatabaseError as error:
        code, message = error.args
        kind = {
            "integrity": IntegrityError,
            "operational": OperationalError,
            "closed": InterfaceError,
            "busy": OperationalError,
            "data": DataError,
            "programming": ProgrammingError,
        }.get(code, DatabaseError)
        raise kind(code, message) from None


class Connection:
    def __init__(self, handle):
        self.handle = handle

    def cursor(self, *, stream=False):
        return Cursor(self, stream)

    def begin(self):
        native(self.handle.begin)

    def commit(self):
        native(self.handle.commit)

    def rollback(self):
        native(self.handle.rollback)

    def close(self):
        native(self.handle.close)

    @property
    def autocommit(self):
        return native(getattr, self.handle, "autocommit")

    @autocommit.setter
    def autocommit(self, value):
        native(setattr, self.handle, "autocommit", value)


class Cursor:
    def __init__(self, connection, stream):
        self.connection = connection
        self._stream = stream
        self._cursor = None
        self._closed = False
        self.arraysize = 1
        self.description = None
        self.rowcount = -1
        self.lastrowid = None
        self.storage = None

    def _check(self):
        if self._closed:
            raise InterfaceError("closed", "Database cursor is closed")

    def _release(self):
        if self._cursor is not None:
            native(self._cursor.close)
            self._cursor = None

    def close(self):
        self._release()
        self._closed = True

    def execute(self, statement, parameters=()):
        self._check()
        self._release()
        self._cursor = native(
            self.connection.handle.execute,
            statement,
            json.dumps(list(parameters), allow_nan=False),
            self._stream,
        )
        if self.storage is not None:
            native(self._cursor.guard, self.storage)
        return self._describe()

    def _describe(self):
        assert self._cursor is not None
        columns = json.loads(native(self._cursor.description))
        self.description = (
            [(name, kind, None, None, None, None, None) for name, kind in columns]
            if columns
            else None
        )
        self.rowcount = self._cursor.rowcount
        self.lastrowid = self._cursor.lastrowid
        return self

    def executemany(self, statement, parameters):
        self._check()
        self._release()
        self._cursor = native(
            self.connection.handle.executemany,
            statement,
            json.dumps([list(values) for values in parameters], allow_nan=False),
        )
        if self.storage is not None:
            native(self._cursor.guard, self.storage)
        return self._describe()

    def fetchone(self):
        values = self.fetchmany(1)
        return values[0] if values else None

    def fetchmany(self, size=None):
        self._check()
        size = self.arraysize if size is None else size
        if size < 0:
            raise ProgrammingError("programming", "Invalid database fetch size")
        if self._cursor is None:
            raise ProgrammingError("programming", "Database cursor has no result")
        rows = []
        while len(rows) < size:
            batch = json.loads(native(self._cursor.fetchmany, min(size - len(rows), 1000)))
            if not batch:
                break
            rows.extend(tuple(row) for row in batch)
        return rows

    def fetchall(self):
        rows = []
        while batch := self.fetchmany(1000):
            rows.extend(batch)
        return rows
