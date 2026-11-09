"""Pinned Warehouse reads and a disposable cache of immutable remote artifacts.

There is no local current version or warehouse writer. Latest is resolved at
Warehouse; children open the exact version selected by admission. A cached version
retains its original endpoint, so changing Terminal's selection cannot redirect
an in-flight download. Removing a file fetches the same artifact again.
"""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import tempfile

from asterion.common_pb2 import ErrorCode
from asterion.warehouse_pb2 import Request, Response
from asterion_lab.warehouse import ParquetTables, Version, canonical, write_durably

IDENTITY = re.compile(r"[0-9a-f]{64}")
CHUNK = 1024 * 1024


class WarehouseError(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class Client:
    """One bounded TLS connection. Read failures are returned, never retried."""
    def __init__(self, origin):
        self.calls = 0
        self.origin, self.stream = origin, None

    def __enter__(self):
        return self

    def __exit__(self, *ignored):
        if self.stream is not None:
            self.stream.close()

    def call(self, operation, **values):
        if operation not in ("get_dataset_version", "read_dataset_file", "get_dataset_summary"):
            raise ValueError("Lab only reads Warehouse datasets")
        self.calls += 1
        request = Request(id=self.calls)
        call = getattr(request, operation)
        call.SetInParent()
        call.CopyFrom(type(call)(**values))
        try:
            if self.stream is None:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                context.minimum_version = ssl.TLSVersion.TLSv1_3
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE  # Trust is the pinned certificate.
                context.load_cert_chain(self.origin["certificate_file"], self.origin["private_key_file"])
                connection = socket.create_connection((self.origin["host"], self.origin["port"]), timeout=10)
                try:
                    self.stream = context.wrap_socket(connection)
                except BaseException:
                    connection.close()
                    raise
                if hashlib.sha256(self.stream.getpeercert(binary_form=True)).hexdigest() != self.origin["fingerprint"]:
                    raise WarehouseError("unavailable", "Warehouse certificate does not match its configured fingerprint")
                self.stream.settimeout(60)
            payload = request.SerializeToString()
            self.stream.sendall(struct.pack("!I", len(payload)) + payload)
            size, = struct.unpack("!I", self.receive(4))
            if not 0 < size <= 2 * CHUNK:
                raise WarehouseError("invalid_data", "invalid Warehouse dataset response size")
            response = Response.FromString(self.receive(size))
        except OSError as error:
            raise WarehouseError("unavailable", f"Warehouse dataset read failed: {error}") from None
        if response.id != request.id:
            raise WarehouseError("invalid_data", "Warehouse dataset response identity mismatch")
        result = response.WhichOneof("result")
        if result == "error":
            code = ErrorCode.Name(response.error.code).removeprefix("ERROR_CODE_").lower()
            raise WarehouseError(code, response.error.message)
        if result != operation:
            raise WarehouseError("invalid_data", "Warehouse returned another dataset operation")
        return getattr(response, operation)

    def receive(self, size):
        data = bytearray()
        while len(data) < size:
            chunk = self.stream.recv(size - len(data))
            if not chunk:
                raise WarehouseError("unavailable", "Warehouse closed the dataset connection")
            data.extend(chunk)
        return bytes(data)


class Endpoint:
    """Read Terminal's saved selection; provider credentials never enter Lab."""
    def __init__(self, terminal_data: Path):
        self.directory = terminal_data

    def selected(self):
        path = self.directory / "warehouse.json"
        if not path.exists():
            raise WarehouseError("unavailable", "no Warehouse is configured")
        try:
            value = json.loads(path.read_text())
            host, port, fingerprint = value["host"], value["port"], value["fingerprint"]
            if (not isinstance(host, str) or not host or len(host) > 253
                    or any(c.isspace() or ord(c) < 32 for c in host)
                    or type(port) is not int or not 1 <= port <= 65535
                    or not isinstance(fingerprint, str) or not IDENTITY.fullmatch(fingerprint)):
                raise ValueError("invalid host, port or certificate fingerprint")
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise WarehouseError("unavailable", f"unreadable Warehouse settings: {path}: {error}") from None
        return dict(host=host, port=port, fingerprint=fingerprint,
                    certificate_file=str(self.directory / "tls/client.crt"),
                    private_key_file=str(self.directory / "tls/client.key"))


@contextmanager
def locked(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


class Dataset(ParquetTables):
    """The one dataset Warehouse offers."""
    provider = "research"

    def __init__(self, directory: Path, history=None, origin=None):
        self.root = directory / "warehouse-cache" / self.provider
        self.history, self.origin = history, origin

    def current(self):
        if self.origin is None and self.history is None:
            raise ValueError("research children require a fixed Warehouse dataset version")
        return self.fetch("", self.origin if self.origin is not None else self.history.selected())

    def version(self, identity):
        if not IDENTITY.fullmatch(identity):
            raise ValueError("dataset version must be a 64-digit lowercase SHA-256 identity")
        path = self.root / "versions" / f"{identity}.json"
        if path.exists():
            return Version(identity, json.loads(path.read_text()))
        if self.origin is None and self.history is None:
            raise ValueError(f"unreadable dataset version: {path}; select this exact version from Warehouse again")
        return self.fetch(identity, self.origin if self.origin is not None else self.history.selected())

    def fetch(self, identity, origin):
        partitions, after, selected = {}, "", identity
        with Client(origin) as client:
            while True:
                page = client.call("get_dataset_version", after_key=after,
                                   dataset=dict(version=selected))
                dataset = page.dataset
                if (not selected and not after and not dataset.version
                        and not page.partitions and not page.next_key):
                    return Version("", dict(provider=self.provider, created_ms=0, partitions={}))
                if not IDENTITY.fullmatch(dataset.version) or selected and dataset.version != selected:
                    raise WarehouseError("invalid_data", "Warehouse returned another dataset version")
                if not after:
                    selected = dataset.version
                    manifest = dict(provider=self.provider, parent=page.parent or None,
                                    created_ms=dataset.created_ms, origin=origin, partitions=partitions)
                for part in page.partitions:
                    if part.key <= after or not IDENTITY.fullmatch(part.sha256):
                        raise WarehouseError("invalid_data", "invalid Warehouse partition page")
                    after = part.key
                    partitions[part.key] = dict(file=f"files/{part.sha256}.parquet",
                                               sha256=part.sha256, rows=part.rows, days=list(part.days))
                if not page.next_key:
                    break
                if not page.partitions or page.next_key != after:
                    raise WarehouseError("invalid_data", "Warehouse partition cursor did not advance")
        path = self.root / "versions" / f"{selected}.json"
        with locked(self.root / "locks" / f"{selected}.lock"):
            # The first complete read owns the origin of this cached version.
            if not path.exists():
                write_durably(path, canonical(manifest))
        return self.version(selected)

    def ensure(self, version, key):
        part = version.manifest["partitions"][key]
        path = self.root / part["file"]
        if path.exists():
            return
        with locked(self.root / "locks" / f"{part['sha256']}.lock"):
            if path.exists():
                return
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".download-", delete=False) as output:
                    temporary = Path(output.name)
                    digest, offset, size = hashlib.sha256(), 0, None
                    with Client(version.manifest["origin"]) as client:
                        while size is None or offset < size:
                            item = client.call("read_dataset_file", key=key, offset=offset,
                                               dataset=dict(version=version.identity))
                            if ((item.sha256, item.offset) != (part["sha256"], offset)
                                    or size is not None and item.size != size
                                    or not item.data or len(item.data) > CHUNK
                                    or offset + len(item.data) > item.size):
                                raise WarehouseError("invalid_data", "Warehouse returned inconsistent partition bytes")
                            size = item.size
                            output.write(item.data)
                            digest.update(item.data)
                            offset += len(item.data)
                    if digest.hexdigest() != part["sha256"]:
                        raise WarehouseError("invalid_data", "Warehouse partition digest mismatch")
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, path)
                directory = os.open(path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)

    def query(self, version, sql, tables, parameters=(), **scope):
        # Only the partitions this query reads are downloaded.
        for table in tables:
            for key in version.keys(table, **scope):
                self.ensure(version, key)
        return super().query(version, sql, tables, parameters, **scope)

    def read(self, version, key):
        self.ensure(version, key)
        return super().read(version, key)
