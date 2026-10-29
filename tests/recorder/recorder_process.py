"""The recorder logs in, reads the catalog and records with no client, against
the test-only CTP libraries; clients only read its status and files. A second
instance continues the recording and reports the first one's segment.

    recorder_process.py <asterion_recorder> <test ctp md library> <test ctp trader library>
"""
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

recorder, md, trader = sys.argv[1:4]


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


def ask(endpoint, number, body=b""):
    request = b"\x08\x01" + field(2, b"probe") + field(number, body)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(str(endpoint))
        connection.sendall(len(request).to_bytes(4, "big") + request)
        header = connection.recv(4, socket.MSG_WAITALL)
        payload = connection.recv(int.from_bytes(header, "big"), socket.MSG_WAITALL)
    response = decode(payload)
    assert 20 not in response, decode(response[20][0])
    return decode(response[number][0])


def text(fields, number):
    return fields.get(number, [b""])[0].decode()


def start(root, endpoint):
    return subprocess.Popen(
        [recorder, "--directory", str(root / "recorder"), "--config", str(root / "recorder.json"),
         "--ctp-library", md, "--ctp-catalog-library", trader, "--endpoint", str(endpoint)],
        env=env, stderr=subprocess.PIPE, text=True)


def wait_for(process, endpoint, predicate, what):
    deadline = time.monotonic() + 40
    while True:
        assert process.poll() is None, process.stderr.read()
        if endpoint.exists():
            status = ask(endpoint, 10)
            if predicate(status):
                return status
        assert time.monotonic() < deadline, f"{what}: {status if endpoint.exists() else 'no socket'}"
        time.sleep(.2)


with tempfile.TemporaryDirectory(prefix="ast-recorder-") as folder:
    root = Path(folder)
    (root / "recorder").mkdir()
    (root / "credentials").mkdir(mode=0o700)
    env = dict(os.environ, CREDENTIALS_DIRECTORY=str(root / "credentials"),
               ASTERION_LOG_DIRECTORY=str(root / "logs"))
    (root / "recorder.json").write_text(json.dumps({
        "market_front": "tcp://127.0.0.1:1", "trade_front": "tcp://127.0.0.1:1",
        "broker": "test", "user": "fixture", "app_id": "app"}))

    # A missing credential refuses the start and names it.
    refused = start(root, root / "r.sock")
    _, error = refused.communicate(timeout=15)
    assert refused.returncode != 0 and "asterion.ctp.password" in error, error

    (root / "credentials/asterion.ctp.password").write_text("fixture-only-secret\n")
    (root / "credentials/asterion.ctp.auth_code").write_text("fixture-auth")
    first_endpoint = root / "first.sock"
    first = start(root, first_endpoint)
    try:
        # Nobody asks: the catalog is read and every listed contract subscribed.
        status = wait_for(first, first_endpoint,
                          lambda s: s.get(9, [0])[0] > 0 and s.get(10, [0])[0] > 0,
                          "the catalog was not read or no quote arrived")
        assert text(status, 4) == "connected", status
        assert text(status, 6) == "ready", status
        first_instance = text(status, 1)
    finally:
        first.send_signal(signal.SIGTERM)
        first.wait(timeout=30)
    assert first.returncode == 0, first.stderr.read()

    second_endpoint = root / "second.sock"
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
        bars = [decode(bar) for bar in minutes.get(4, [])]
        # The bar forming at the stop is written, marked partial.
        assert bars and text(bars[-1], 5) == "3510" and bars[-1].get(9) == [1], bars
    finally:
        second.send_signal(signal.SIGTERM)
        second.wait(timeout=30)

    logs = "".join(path.read_text() for path in (root / "logs").rglob("*.log"))
    assert "catalog.applied" in logs, logs
    for secret in ("fixture-only-secret", "fixture-auth"):
        assert secret not in logs and secret not in second.stderr.read()
    print("recorded", trading_day, "in", len(segments), "segments;", len(bars), "minutes of rb2610")
