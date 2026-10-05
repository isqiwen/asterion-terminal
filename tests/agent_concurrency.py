"""A stalled TLS handshake or authenticated partial frame cannot block Agent status."""
import json
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time


def daily(path):
    """The Agent writes the transport log as <stem>_YYYY-MM-DD<suffix>."""
    return next(path.parent.glob(f"{path.stem}_????-??-??{path.suffix}"))

agent_path, certificates, mode = sys.argv[1:]
assert mode in ("handshake", "frame", "overload")
def varint(value):
    output = bytearray()
    while value >= 128:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return bytes(output)
def field(number, data):
    return varint(number << 3 | 2) + varint(len(data)) + data

def fields(data):
    offset = 0
    def integer():
        nonlocal offset
        result = shift = 0
        while offset < len(data) and shift < 64:
            value = data[offset]; offset += 1
            result |= (value & 127) << shift
            if value < 128:
                return result
            shift += 7
        raise AssertionError("malformed protobuf in test response")
    result = {}
    while offset < len(data):
        key = integer(); number, wire = key >> 3, key & 7
        assert number not in result
        if wire == 0:
            result[number] = integer()
        else:
            assert wire == 2
            size = integer(); end = offset + size
            assert end <= len(data)
            result[number] = data[offset:end]; offset = end
    return result

with tempfile.TemporaryDirectory(prefix="ast-agent-concurrency-", ignore_cleanup_errors=True) as folder:
    root = Path(folder); (root / "state").mkdir()
    subprocess.run([certificates, folder], check=True)
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0)); port = reserve.getsockname()[1]
    log = root / "state/logs/transport.log"
    process = subprocess.Popen([agent_path, "--directory", str(root / "state"), "--bind", "127.0.0.1", "--port", str(port), "--tls-ca", str(root / "ca.crt"), "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    context = ssl.create_default_context(cafile=str(root / "ca.crt"))
    context.load_cert_chain(str(root / "client.crt"), str(root / "client.key"))
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    def connect(ctx=context):
        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        try:
            return ctx.wrap_socket(raw, server_hostname="localhost")
        except BaseException:
            raw.close(); raise
    def status(identity, ctx=context):
        request = b"\x08\x01" + field(2, identity.encode()) + field(10, b"")
        with connect(ctx) as channel:
            channel.sendall(struct.pack("!I", len(request)) + request)
            def receive(size):
                data = b""
                while len(data) < size:
                    part = channel.recv(size - len(data)); assert part
                    data += part
                return data
            size = struct.unpack("!I", receive(4))[0]
            assert 0 < size <= 16 * 1024 * 1024
            response = fields(receive(size))
            assert response[1] == 1 and response[2] == identity.encode(), response
            assert set(response) == {1, 2, 10}, response
            return fields(response[10])
    stalled = None
    overloaded = []
    try:
        deadline = time.monotonic() + 10
        while True:
            assert process.poll() is None, "Agent exited before readiness"
            try:
                first = status("ready"); break
            except (OSError, AssertionError):
                if time.monotonic() >= deadline: raise
                time.sleep(.05)
        if mode == "overload":
            overloaded = [socket.create_connection(("127.0.0.1", port), timeout=3) for _ in range(32)]
            rejected = set()
            limit = time.monotonic() + 4
            while len(rejected) < 16 and time.monotonic() < limit:
                readable, _, _ = select.select([s for s in overloaded if s not in rejected], [], [], .1)
                for channel in readable:
                    assert channel.recv(1) == b"", "unauthenticated overload peer received application data"
                    rejected.add(channel)
            assert len(rejected) >= 16, f"bounded admission did not close excess peers: {len(rejected)}"
            for channel in overloaded: channel.close()
            overloaded.clear()
            recovered = time.monotonic() + 5
            while True:
                try:
                    assert status("after-overload")[1] == first[1]
                    break
                except OSError:
                    if time.monotonic() >= recovered: raise
                    time.sleep(.05)
        if mode == "handshake":
            overloaded = [socket.create_connection(("127.0.0.1", port), timeout=3) for _ in range(8)]
        elif mode == "frame":
            stalled = connect()
            stalled.sendall(b"\x00") # Incomplete length header.
        started = time.monotonic()
        current = status("while-stalled")
        assert current[1] == first[1] and time.monotonic() - started < 3
        # Parallel reception must not weaken the certificate gate.
        stranger = ssl.create_default_context(cafile=str(root / "ca.crt"))
        stranger.load_cert_chain(str(root / "stranger.crt"), str(root / "stranger.key"))
        try:
            status("untrusted", stranger)
        except (OSError, AssertionError):
            pass
        else:
            raise AssertionError("untrusted client reached Agent status")
        assert status("after-rejection")[1] == first[1]
        # Diagnostics expose only server-generated identifiers and enum-like
        # stages; malformed or truncated payloads must never enter logs.
        secret = b"credential-do-not-log"
        with connect() as broken:
            broken.sendall(struct.pack("!I", len(secret) + 100) + secret)
        with connect() as malformed:
            malformed.sendall(struct.pack("!I", 1) + b"\xff")
            assert malformed.recv(1) == b""
        until = time.monotonic() + 5
        while True:
            records = [json.loads(line) for line in daily(log).read_text().splitlines()]
            failures = [r for r in records if r["event"] == "rpc.connection_failed"]
            if failures:
                break
            if time.monotonic() >= until:
                raise AssertionError(records)
            time.sleep(.05)
        # Transport now uses the shared RpcHost diagnostics. Protect payload secrecy
        # and explicit rejection, rather than the removed per-connection host stages.
        assert all(set(r["fields"]) == {"code", "failures"} for r in failures), failures
        assert all(secret.decode() not in path.read_text() for path in (root / "state/logs").glob("*.log"))
    finally:
        if stalled is not None: stalled.close()
        for channel in overloaded: channel.close()
        process.kill()
        _, diagnostic = process.communicate(timeout=15)
        if diagnostic: print(diagnostic.decode(errors="replace"), file=sys.stderr)
print(f"Agent {mode} stall isolated; authenticated status and certificate rejection verified")
