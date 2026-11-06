"""The recorder logs in, reads the catalog and records with no client, against
the test-only CTP libraries; clients read its status and files over pinned TLS. A second
instance continues the recording and reports the first one's segment.

    recording_process.py <asterion_quotes> <test ctp md library> <test ctp trader library>
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).parents[1] / "support"))
from history_installation import install_program
import history_tls

market, md, trader = sys.argv[1:4]
executable = "asterion_quotes"


def varint(value):
    out = bytearray()
    while True:
        byte, value = value & 0x7F, value >> 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def field(number, payload):
    return varint(number << 3 | 2) + varint(len(payload)) + payload


def decode(data):
    """Top-level fields of a Protobuf message: number -> list of int or bytes."""
    fields, at = {}, 0
    while at < len(data):
        key, at = read_varint(data, at)
        number, kind = key >> 3, key & 7
        if kind == 0:
            value, at = read_varint(data, at)
        elif kind == 2:
            size, at = read_varint(data, at)
            value, at = data[at:at + size], at + size
        else:
            raise AssertionError(f"unexpected wire type {kind}")
        fields.setdefault(number, []).append(value)
    return fields


def read_varint(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.recv(size - len(result))
        if not part: return bytes(result)
        result.extend(part)
    return bytes(result)


def ask(port, number, body=b"", magic=b"ASTR", rejected=False, role="reader"):
    """One call by its field number; its result, or its error when rejected."""
    request = b"\x08\x01" + (field(number, body) if number else b"")
    with (history_tls.connect(tls, port, role) if magic == b"ASTR" else socket.create_connection(("127.0.0.1", port), timeout=10)) as connection:
        connection.sendall((b"" if magic == b"ASTR" else magic) + len(request).to_bytes(4, "big") + request)
        try:
            header = read_exact(connection, 4)
        except ConnectionResetError:
            return None
        if not header:
            return None
        payload = read_exact(connection, int.from_bytes(header, "big"))
    response = decode(payload)
    if rejected:
        assert ERROR in response, response
        return decode(response[ERROR][0])
    assert ERROR not in response, decode(response[ERROR][0])
    return decode(response[number][0])


# Field numbers of quotes.proto.
ERROR, GET_STATUS, STREAM_QUOTES, GET_MINUTES = 2, 10, 11, 12
GET_DATASET_VERSION, START_ACQUISITION, GET_ACQUISITION_TASK = 21, 31, 32
FEED_CONNECTED, CATALOG_LOADING, CATALOG_READY, CATALOG_FAILED = 5, 2, 4, 5
INVALID_REQUEST, TABLE_MINUTES, TASK_SUCCEEDED = 1, 4, 3


def number(fields, at, default=0):
    return fields.get(at, [default])[0]


def feed_state(status):
    return number(decode(status[3][0]), 1) if 3 in status else 0


def catalog_state(status):
    return number(decode(status[4][0]), 1) if 4 in status else 0


def instrument(exchange, symbol):
    return field(1, exchange.encode()) + field(2, symbol.encode())


@contextmanager
def watch_stream(port, instruments):
    body = b"".join(field(1, instrument(*item)) for item in instruments)
    request = b"\x08\x01" + field(STREAM_QUOTES, body)
    with history_tls.connect(tls, port) as stream:
        stream.sendall(len(request).to_bytes(4, "big") + request)
        yield stream


def watch_frame(stream, kind=2):
    """The next frame's board: kind 1 is full, 2 changed."""
    def read(size):
        data = bytearray()
        while len(data) < size:
            part = stream.recv(size - len(data))
            assert part, "watch disconnected"
            data.extend(part)
        return bytes(data)
    response = decode(read(int.from_bytes(read(4), "big")))
    assert ERROR not in response, response
    frame = decode(response[STREAM_QUOTES][0])
    assert kind in frame, frame
    return decode(frame[kind][0])


def rows(board):
    members = [decode(decode(row)[1][0]) for row in board.get(2, [])]
    return [(text(row, 1), text(row, 2)) for row in members]


def check_client_isolation(port):
    rb = ("SHFE", "rb2610")
    absent = ("SHFE", "rb2611")
    with watch_stream(port, [rb]) as a, watch_stream(port, [absent]) as b, watch_stream(port, []) as all_rows:
        first_a, first_b, full = watch_frame(a, 1), watch_frame(b, 1), watch_frame(all_rows, 1)
        assert rows(first_a) == [rb], first_a
        assert rows(first_b) == [], first_b
        assert first_a[2] == full[2], (first_a, full)  # one shared quote, not per-client feeds
        # Each stream continues with its own changes after its own full frame.
        watch_frame(a)
        assert not watch_frame(b).get(2)
        # A selection change replaces only B's stream and starts it with a full frame.
        b.close()
        with watch_stream(port, [rb]) as changed_b:
            assert rows(watch_frame(changed_b, 1)) == [rb]
        watch_frame(a)
        duplicate = field(1, instrument(*rb)) * 2
        denied = ask(port, STREAM_QUOTES, duplicate, rejected=True)
        assert number(denied, 1) == INVALID_REQUEST, denied
        watch_frame(a)
    with watch_stream(port, []) as shared:
        assert rows(watch_frame(shared, 1)) == [rb]


def text(fields, number):
    return fields.get(number, [b""])[0].decode()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def install(root):
    """The recorder's installation directory, as on a recording host."""
    for folder in ("bin", "lib", "config", "logs", "data"):
        (root / folder).mkdir()
    install_program(market, root / "bin" / executable)
    if sys.platform == "linux":
        install_program(Path(market).parent / "asterion_crash_reporter", root / "bin/asterion_crash_reporter")
    shutil.copy2(md, root / "lib/ctp-md.so")
    shutil.copy2(trader, root / "lib/ctp-trader.so")
    shutil.copy2(Path(__file__).parents[2] / "apps/services/quotes/config/logging.json", root / "config/logging.json")


def start(root, port, user="fixture"):
    """Everything comes from the installation directory and its configuration file."""
    (root / "config" / "quotes.json").write_text(json.dumps({
        "bind": "127.0.0.1", "port": port, "data": str(root / "data"),
        "clients": [dict(fingerprint=c["fingerprint"]) for c in history_tls.clients(tls)],
        "counter": {"market_front": "tcp://127.0.0.1:1", "trade_front": "tcp://127.0.0.1:1",
                    "broker": "test", "user": user, "app_id": "app"}}))
    command = [str(root / "bin" / executable)]
    return subprocess.Popen(command, env=env, stderr=subprocess.PIPE, text=True)


def wait_for(process, port, predicate, what):
    deadline = time.monotonic() + 40
    status = None
    while True:
        assert process.poll() is None, process.stderr.read()
        try:
            status = ask(port, GET_STATUS)
            if predicate(status):
                return status
        except ConnectionRefusedError:
            pass
        assert time.monotonic() < deadline, f"{what}: {status}"
        time.sleep(.2)


def collecting(status, catalog=CATALOG_READY):
    return (number(status, 5) > 0 and number(status, 6) > 0
            and feed_state(status) == FEED_CONNECTED and catalog_state(status) == catalog)


with tempfile.TemporaryDirectory(prefix="ast-market-") as folder:
    root = Path(folder)
    install(root)
    tls = history_tls.prepare(root)
    (root / "credentials").mkdir(mode=0o700)
    # History writes its logs to <installation>/logs by itself.
    env = dict(os.environ, CREDENTIALS_DIRECTORY=str(root / "credentials"))

    # A missing credential refuses the start and names it.
    refused = start(root, free_port())
    _, error = refused.communicate(timeout=15)
    assert refused.returncode != 0 and "asterion.ctp.password" in error, error

    (root / "credentials/asterion.ctp.password").write_text("fixture-only-secret\n")
    (root / "credentials/asterion.ctp.auth_code").write_text("fixture-auth")
    first_endpoint = free_port()
    first = start(root, first_endpoint, "steady")
    try:
        # Nobody asks: the catalog is read and every listed contract subscribed.
        status = wait_for(first, first_endpoint, collecting, "the catalog was not read or no quote arrived")
        started = number(status, 2)
        # A second process cannot write the same installation, even on another port.
        duplicate = start(root, free_port())
        _, rejection = duplicate.communicate(timeout=15)
        assert duplicate.returncode != 0 and "quotes.lock" in rejection, rejection
        assert "held by another process" in rejection, rejection
        assert number(ask(first_endpoint, GET_STATUS), 2) == started
        with watch_stream(first_endpoint, []) as viewer:  # one frame, then the viewer closes
            board = watch_frame(viewer, 1)
        assert number(decode(board[1][0]), 1) == FEED_CONNECTED and board.get(2), board
        live_minutes = ask(first_endpoint, GET_MINUTES, instrument("SHFE", "rb2610"))
        assert live_minutes.get(6), live_minutes
        denied = ask(first_endpoint, GET_MINUTES, rejected=True)
        assert number(denied, 1) == INVALID_REQUEST, denied
        denied = ask(first_endpoint, 0, rejected=True)
        assert number(denied, 1) == INVALID_REQUEST, denied
        check_client_isolation(first_endpoint)
        if len(sys.argv) > 4:
            if sys.argv[4] == "--terminal":
                subprocess.run([sys.executable, str(Path(__file__).parents[1] / "quotes/quotes_process.py"),
                                sys.argv[5], str(first_endpoint), str(tls)], check=True, timeout=30)
            else:
                subprocess.run([sys.argv[4], str(first_endpoint), str(tls)], check=True, timeout=20)
        # Each request closes its client. No viewer owns the provider lifecycle.
        after = ask(first_endpoint, GET_STATUS)
        assert feed_state(after) == FEED_CONNECTED and number(after, 5) == number(status, 5), after
        # A plaintext connection is closed unanswered.
        assert ask(first_endpoint, GET_STATUS, magic=b"HTTP") is None
    finally:
        first.send_signal(signal.SIGTERM)
        first.wait(timeout=30)
    assert first.returncode == 0, first.stderr.read()

    # A connection that outlasts a trading day reads the catalog for the new day.
    endpoint = free_port()
    continuing = start(root, endpoint, "next-day")
    try:
        cache = root / "data/ctp-catalog.pb"
        begun = time.time_ns()
        deadline = time.monotonic() + 40
        reads = set()
        while len(reads) < 2:
            assert continuing.poll() is None, continuing.stderr.read()
            assert time.monotonic() < deadline, reads
            if cache.exists() and cache.stat().st_mtime_ns > begun:
                reads.add(cache.stat().st_mtime_ns)
            time.sleep(.05)
    finally:
        continuing.send_signal(signal.SIGTERM)
        continuing.wait(timeout=30)

    # A refresh that stalls or fails must not erase the last usable catalog.
    # The restarted host can still serve quotes while reporting the query state.
    for mode, state in (("catalog-stall", CATALOG_LOADING), ("catalog-error", CATALOG_FAILED)):
        endpoint = free_port()
        restarted = start(root, endpoint, mode)
        try:
            wait_for(restarted, endpoint, lambda s: collecting(s, state),
                     "cached catalog did not resume collection")
            with watch_stream(endpoint, []) as viewer:
                cached = decode(watch_frame(viewer, 1)[3][0])
            assert cached.get(2), cached
        finally:
            restarted.send_signal(signal.SIGTERM)
            restarted.wait(timeout=30)
        assert restarted.returncode == 0, restarted.stderr.read()

    # A provider burst that overflows the ingress queue ends the process for systemd to restart.
    flooded = start(root, free_port(), user="flood")
    flooded.communicate(timeout=60)
    assert flooded.returncode != 0, flooded.returncode

    logs = "".join(path.read_text() for path in (root / "logs").rglob("*.log"))
    assert "[error] [asterion_quotes] [" in logs, logs  # one text line per record
    assert not logs.lstrip().startswith("{"), logs
    for secret in ("fixture-only-secret", "fixture-auth"):
        assert secret not in logs and secret not in continuing.stderr.read()
