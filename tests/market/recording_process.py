"""The recorder logs in, reads the catalog and records with no client, against
the test-only CTP libraries; clients read its status and files over pinned TLS. A second
instance continues the recording and reports the first one's segment.

    recording_process.py <asterion_market> <test ctp md library> <test ctp trader library>
"""
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
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
from market_installation import install_program
import market_tls

market, md, trader = sys.argv[1:4]
executable = "asterion_market"


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


def ask(port, number, body=b"", magic=b"ASTR", raw=False, rejected=False):
    number = number if raw else {10: 19, 11: 20, 12: 21}.get(number, number)
    request = b"\x08\x01" + field(2, b"market-data") + field(3, b"probe") + field(number, body)
    with (market_tls.connect(tls, port) if magic == b"ASTR" else socket.create_connection(("127.0.0.1", port), timeout=10)) as connection:
        connection.sendall((b"" if magic == b"ASTR" else magic) + len(request).to_bytes(4, "big") + request)
        try:
            header = read_exact(connection, 4)
        except ConnectionResetError:
            return None
        if not header:
            return None
        payload = read_exact(connection, int.from_bytes(header, "big"))
    response = decode(payload)
    error_field = 12
    if rejected:
        assert error_field in response, response
        return decode(response[error_field][0])
    assert error_field not in response, decode(response[error_field][0])
    result = {19: 15, 20: 16, 21: 17, 13: 10, 14: 10, 15: 11, 16: 13, 18: 14}.get(number, 10)
    return decode(response[result][0])


@contextmanager
def watch_stream(port, instruments):
    body = b"".join(field(1, field(1, venue.encode()) + field(2, symbol.encode()))
                    for venue, symbol in instruments)
    request = b"\x08\x01" + field(2, b"market-data") + field(3, b"viewer") + field(14, body)
    with market_tls.connect(tls, port) as stream:
        stream.sendall(len(request).to_bytes(4, "big") + request)
        yield stream


def watch_frame(stream):
    def read(size):
        data = bytearray()
        while len(data) < size:
            part = stream.recv(size - len(data))
            assert part, "watch disconnected"
            data.extend(part)
        return bytes(data)
    response = decode(read(int.from_bytes(read(4), "big")))
    assert 12 not in response, response
    return decode(response[10][0])


def watch_members(snapshot, field_number):
    rows = [decode(row) for row in snapshot.get(field_number, [])]
    if field_number == 6:  # subscriptions wrap the instrument in field 1
        rows = [decode(row[1][0]) for row in rows]
    return [(text(row, 1), text(row, 2)) for row in rows]


def check_client_isolation(port, instance):
    rb = ("SHFE", "rb2610")
    absent = ("SHFE", "rb2611")
    with watch_stream(port, [rb]) as a, watch_stream(port, [absent]) as b, watch_stream(port, []) as all_rows:
        first_a, first_b, full = watch_frame(a), watch_frame(b), watch_frame(all_rows)
        for snapshot in (first_a, first_b, full):
            assert text(snapshot, 1) == instance, snapshot
            assert not snapshot.get(10) and not snapshot.get(11) and not snapshot.get(12), snapshot
        assert watch_members(first_a, 6) == [rb], first_a
        assert watch_members(first_b, 6) == [], first_b
        assert watch_members(first_a, 8) == [rb] and watch_members(first_b, 8) == [absent]
        assert watch_members(full, 8) == [], full
        assert first_a[6] == full[6], (first_a, full)  # one shared quote, not per-client feeds
        # Reading one stream does not advance another stream's baseline.
        next_a = watch_frame(a)
        assert next_a.get(11) == [1] and next_a.get(12) == first_a.get(4), next_a
        next_b = watch_frame(b)
        assert next_b.get(11) == [1] and next_b.get(12) == first_b.get(4), next_b
        assert not next_b.get(6) and watch_members(next_b, 8) == [absent], next_b
        # A selection change replaces only B's stream and gives B a fresh baseline.
        b.close()
        with watch_stream(port, [rb]) as changed_b:
            changed = watch_frame(changed_b)
            assert not changed.get(11) and not changed.get(10), changed
            assert watch_members(changed, 6) == [rb], changed
        following_a = watch_frame(a)
        assert following_a.get(12) == next_a.get(4), following_a
        assert watch_members(following_a, 8) == [rb], following_a
        duplicate = field(1, field(1, b"SHFE") + field(2, b"rb2610")) * 2
        denied = ask(port, 14, duplicate, rejected=True)
        assert text(denied, 1) == "invalid_request", denied
        assert watch_frame(a).get(12) == following_a.get(4)
    shared = ask(port, 13)
    assert not shared.get(8) and watch_members(shared, 6) == [rb], shared


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
    shutil.copy2(md, root / "lib/ctp-md.so")
    shutil.copy2(trader, root / "lib/ctp-trader.so")
    shutil.copy2(Path(__file__).parents[2] / "config/logging.json", root / "config/logging.json")


def start(root, port, user="fixture"):
    """Everything comes from the installation directory and its configuration file."""
    (root / "config" / "market.json").write_text(json.dumps({
        "bind": "127.0.0.1", "port": port, "data": str(root / "data"),
        "recording": {"tick_days": 30}, "clients": market_tls.clients(tls),
        "acquisition": {"exchanges": [], "tushare": None},
        "counter": {"market_front": "tcp://127.0.0.1:1", "trade_front": "tcp://127.0.0.1:1",
                    "broker": "test", "user": user, "app_id": "app"}}))
    command = [str(root / "bin" / executable)]
    command += ["--installation", str(root)]
    return subprocess.Popen(command, env=env, stderr=subprocess.PIPE, text=True)


def wait_for(process, port, predicate, what):
    deadline = time.monotonic() + 40
    status = None
    while True:
        assert process.poll() is None, process.stderr.read()
        try:
            status = ask(port, 10)
            if predicate(status):
                return status
        except ConnectionRefusedError:
            pass
        assert time.monotonic() < deadline, f"{what}: {status}"
        time.sleep(.2)


with tempfile.TemporaryDirectory(prefix="ast-market-") as folder:
    root = Path(folder)
    install(root)
    tls = market_tls.prepare(root)
    (root / "credentials").mkdir(mode=0o700)
    # The recorder writes its logs to <installation>/logs by itself.
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
        status = wait_for(first, first_endpoint,
                          lambda s: s.get(9, [0])[0] > 0 and s.get(10, [0])[0] > 0
                              and text(s, 6) == "ready" and text(s, 4) == "connected",
                          "the catalog was not read or no quote arrived")
        assert text(status, 4) == "connected", status
        assert text(status, 6) == "ready", status
        first_instance = text(status, 1)
        # A second process cannot write the same installation, even on another port.
        duplicate = start(root, free_port())
        _, rejection = duplicate.communicate(timeout=15)
        assert duplicate.returncode != 0 and "market.lock" in rejection, rejection
        assert "held by another process" in rejection, rejection
        assert text(ask(first_endpoint, 10), 1) == first_instance
        snapshot = ask(first_endpoint, 14)  # receive a watch frame then close the viewer
        assert text(snapshot, 1) == first_instance and text(snapshot, 2) == "connected", snapshot
        assert snapshot.get(6), snapshot
        instrument = field(1, b"SHFE") + field(2, b"rb2610")
        live_minutes = ask(first_endpoint, 18, field(1, instrument))
        assert live_minutes.get(5), live_minutes
        events = ask(first_endpoint, 16, b"\x18\x20")
        assert events.get(6), events
        denied = ask(first_endpoint, 12, raw=True, rejected=True)
        assert text(denied, 1) == "invalid_request", denied
        denied = ask(first_endpoint, 11, raw=True, rejected=True)
        assert text(denied, 1) == "invalid_request", denied
        check_client_isolation(first_endpoint, first_instance)
        if len(sys.argv) > 4:
            if sys.argv[4] == "--terminal":
                subprocess.run([sys.executable, str(Path(__file__).parents[1] / "market/market_process.py"),
                                sys.argv[5], str(first_endpoint), str(tls)], check=True, timeout=30)
            else:
                subprocess.run([sys.argv[4], str(first_endpoint), str(tls)], check=True, timeout=20)
        # Each request closes its client. No viewer owns the provider lifecycle.
        after = ask(first_endpoint, 10)
        assert text(after, 4) == "connected" and after.get(9) == status.get(9), after
        # A plaintext connection is closed unanswered.
        assert ask(first_endpoint, 10, magic=b"HTTP") is None
    finally:
        first.send_signal(signal.SIGTERM)
        first.wait(timeout=30)
    assert first.returncode == 0, first.stderr.read()

    second_endpoint = free_port()
    second = start(root, second_endpoint)
    try:
        wait_for(second, second_endpoint, lambda s: s.get(10, [0])[0] > 0, "no quote after restart")
        days = [decode(day) for day in ask(second_endpoint, 11).get(1, [])]
        assert len(days) == 1, days
        day = days[0]
        assert day.get(2, [0])[0] >= 1 and day.get(3, [0])[0] >= 1, day  # tick and minute files
        segments = [decode(segment) for segment in day.get(5, [])]
        ended = [s for s in segments if text(s, 1) == first_instance]
        assert len(ended) == 1 and text(ended[0], 6) == "stopped", segments
        trading_day = text(day, 1)
        beijing = datetime.now(timezone(timedelta(hours=8))).strftime("%Y%m%d")
        assert trading_day == beijing, (trading_day, beijing)
        query = field(1, trading_day.encode()) + field(2, b"SHFE") + field(3, b"rb2610")
        minutes = ask(second_endpoint, 12, query)
        assert minutes == ask(second_endpoint, 12, query), "clients must read the same recorded data"
        bars = [decode(bar) for bar in minutes.get(4, [])]
        # The bar forming at the stop is written, marked partial.
        assert bars and text(bars[-1], 5) == "3510" and bars[-1].get(9) == [1], bars
    finally:
        second.send_signal(signal.SIGTERM)
        second.wait(timeout=30)

    # A refresh that stalls or fails must not erase the last usable catalog.
    # The restarted host can still collect and record while reporting the query state.
    for mode, phase in (("catalog-stall", "loading"), ("catalog-error", "error")):
        endpoint = free_port()
        restarted = start(root, endpoint, mode)
        try:
            recovered = wait_for(restarted, endpoint,
                lambda s: s.get(9, [0])[0] > 0 and s.get(10, [0])[0] > 0
                    and text(s, 6) == phase and text(s, 4) == "connected", "cached catalog did not resume collection")
            assert text(recovered, 4) == "connected", recovered
            cached = decode(ask(endpoint, 13)[7][0])
            assert cached.get(5), cached
        finally:
            restarted.send_signal(signal.SIGTERM)
            restarted.wait(timeout=30)
        assert restarted.returncode == 0, restarted.stderr.read()

    # A provider burst that overflows the ingress queue ends the process for systemd to restart.
    flooded = start(root, free_port(), user="flood")
    flooded.communicate(timeout=60)
    assert flooded.returncode != 0, flooded.returncode

    logs = "".join(path.read_text() for path in (root / "logs").rglob("*.log"))
    assert " info asterion_market " in logs, logs  # one text line per record
    assert not logs.lstrip().startswith("{"), logs
    for secret in ("fixture-only-secret", "fixture-auth"):
        assert secret not in logs and secret not in second.stderr.read()
    print("recorded", trading_day, "in", len(segments), "segments;", len(bars), "minutes of rb2610")
