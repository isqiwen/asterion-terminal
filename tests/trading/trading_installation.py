"""An installed account instance: only the configured Terminal is admitted, by
its certificate; TLS admission is bounded, preserves existing business capacity
and recovers; SIGTERM stops it cleanly.

    trading_installation.py <asterion_trading> <asterion_test_certificates> <test ctp trader>
"""
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import os
from contextlib import ExitStack

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from trading_installation import Programs, install  # noqa: E402

# Sanitizer builds run several times slower.
SCALE = float(os.environ.get("ASTERION_TIMING_SCALE", "1"))

programs = Programs(*sys.argv[1:4])


def response_kind(response):
    at = 0
    while at < len(response):
        key = response[at]
        at += 1
        if key & 7 == 0:
            while response[at] & 0x80:
                at += 1
            at += 1
            continue
        length = shift = 0
        while True:
            byte = response[at]; at += 1
            length |= (byte & 127) << shift
            if not byte & 128: break
            shift += 7
        at += length
        if key >> 3 >= 2:
            return key >> 3


def heartbeat(channel, correlation):
    assert len(correlation) == 3
    # Request(id, get_health={}); the id is the correlation's first byte.
    prefix = b"\x08" + bytes([correlation[0] & 0x7f])
    request = prefix + b"\x52\x00"
    channel.sendall(struct.pack("!I", len(request)) + request)

    def receive(size):
        data = b""
        while len(data) < size:
            part = channel.recv(size - len(data))
            assert part, "trading service closed before heartbeat response"
            data += part
        return data

    size = struct.unpack("!I", receive(4))[0]
    assert 0 < size <= 4096
    response = receive(size)
    assert response.startswith(prefix) and response_kind(response) == 10, response


def result(channel, body):
    """Sends Request(id=1, body); returns the Response's result field number."""
    request = b"\x08\x01" + body
    channel.sendall(struct.pack("!I", len(request)) + request)

    def receive(size):
        data = b""
        while len(data) < size:
            part = channel.recv(size - len(data))
            assert part, "trading service closed the connection"
            data += part
        return data

    response = receive(struct.unpack("!I", receive(4))[0])
    return response_kind(response)


def authenticated_client(installation):
    # Closing silent peers is observed on the host's next admission poll. Only
    # failed handshakes are retried here; no business request has been sent.
    deadline = time.monotonic() + 3 * SCALE
    while True:
        try:
            return installation.client("adm")
        except (ssl.SSLError, ConnectionError, TimeoutError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.05)


def refused(installation, wrap):
    """A connection that must get no application data."""
    raw = socket.create_connection(("127.0.0.1", installation.ports["adm"]), timeout=3)
    try:
        channel = wrap(raw)
        heartbeat(channel, b"bad")
    except (ssl.SSLError, ConnectionError, AssertionError, TimeoutError):
        return
    finally:
        raw.close()
    raise AssertionError("an unlisted client reached the trading service")


with tempfile.TemporaryDirectory(prefix="ast-trading-admission-", ignore_cleanup_errors=True) as folder:
    installation = install(Path(folder) / "trading", programs)
    port = installation.account("adm")
    config = installation.root / "config/accounts/adm.json"
    import fcntl
    def startup_refused(message):
        result = subprocess.run([str(installation.root / 'bin/asterion_trading'), '--account', 'adm'],
            capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and message in result.stderr, result
    with (installation.root / '.program.lock').open() as upgrading:
        fcntl.flock(upgrading, fcntl.LOCK_EX | fcntl.LOCK_NB)
        startup_refused('held by another process')
    (installation.root / '.program-incomplete').touch()
    startup_refused('installation is incomplete')
    (installation.root / '.program-incomplete').unlink()
    import json
    original = json.loads(config.read_text())
    for invalid in ({k: v for k, v in original.items() if k != "broker"},
                    dict(original, environment="unknown"),
                    dict(original, market_front="https://invalid"),
                    dict(original, market_front="tcp://host:99999"),
                    dict(original, broker=dict(original["broker"], password="must-not-be-stored")),
                    dict(original, broker=dict(original["broker"], front="tcp://host:99999"))):
        config.write_text(json.dumps(invalid))
        rejected = subprocess.run([str(installation.root / "bin/asterion_trading"), "--account", "adm"],
            env=dict(os.environ, CREDENTIALS_DIRECTORY=str(installation.root / "test-credentials/adm")),
            capture_output=True, text=True, timeout=10)
        assert rejected.returncode != 0 and str(config) in rejected.stderr
        assert "must-not-be-stored" not in rejected.stderr
    config.write_text(json.dumps(original))
    credentials = installation.root / "test-credentials/adm"
    for name in ("password", "auth_code"):
        file = credentials / name
        saved = file.read_bytes()
        for invalid in (None, b"", b"secret-must-not-leak\0"):
            if invalid is None:
                file.unlink()
            else:
                file.write_bytes(invalid)
            rejected = subprocess.run(
                [str(installation.root / "bin/asterion_trading"), "--account", "adm"],
                env=dict(os.environ, CREDENTIALS_DIRECTORY=str(credentials)),
                capture_output=True, text=True, timeout=10)
            assert rejected.returncode != 0 and str(file) in rejected.stderr
            assert "secret-must-not-leak" not in rejected.stderr
        file.write_bytes(saved)
    process = installation.start("adm")
    try:
        with (installation.root / '.program.lock').open() as upgrading:
            try:
                fcntl.flock(upgrading, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                raise AssertionError('running account did not exclude runtime upgrade')
        with installation.client("adm") as channel:
            heartbeat(channel, b"hb0")
        # Another certificate, none at all, and plain TCP with the magic.
        refused(installation, lambda raw: installation.context("stranger").wrap_socket(raw))
        anonymous = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        anonymous.check_hostname, anonymous.verify_mode = False, ssl.CERT_NONE
        refused(installation, anonymous.wrap_socket)
        def plain(raw):
            raw.sendall(b"ASTR")
            return raw
        refused(installation, plain)
        print("PASS: only the configured Terminal certificate is admitted", flush=True)
        # Reading the account creates no record; a request without a call is refused.
        with installation.client("adm") as channel:
            assert result(channel, b"\x5a\x00") == 11  # get_account: not yet open
            assert result(channel, b"") == 2  # error
        assert not (installation.root / "data/adm/journal.sqlite").exists()
        assert process.poll() is None
        print("PASS: reads and refused requests leave no record", flush=True)
        silent = socket.create_connection(("127.0.0.1", port), timeout=3)
        with ExitStack() as peers:
            peers.enter_context(silent)
            for _ in range(7):
                peers.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            time.sleep(.3)
            started = time.monotonic()
            with installation.client("adm") as channel:
                heartbeat(channel, b"hb1")
            elapsed = time.monotonic() - started
            assert elapsed < 3 * SCALE, f"authenticated client waited {elapsed:.1f}s behind eight silent peers"
        print("PASS: eight silent peers cannot consume the business-worker pool", flush=True)

        for cycle in range(2):
            with authenticated_client(installation) as business:
                heartbeat(business, b"pre")
                with ExitStack() as peers:
                    started = time.monotonic()
                    pending = [peers.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
                               for _ in range(16)]
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as excess:
                        try:
                            assert excess.recv(1) == b"", "excess handshake was not closed"
                        except ConnectionResetError:
                            pass
                    # Default handshakes expire after 10 seconds. Prove that the
                    # excess peer was rejected while all sixteen slots remained
                    # occupied, rather than waiting for an earlier peer to expire.
                    assert time.monotonic() - started < 8, "admission did not reject excess promptly"
                    assert not select.select(pending, [], [], 0)[0], "an admitted silent peer was closed early"
                    heartbeat(business, b"cap")
                heartbeat(business, b"fre")
                with authenticated_client(installation) as recovered:
                    heartbeat(recovered, b"new")
            print(f"PASS: handshake saturation cycle {cycle + 1}: 16 retained, excess closed, business available, capacity recovered", flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, diagnostic = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            _, diagnostic = process.communicate()
            raise AssertionError("trading service did not stop after all test connections closed")
        if diagnostic:
            print(diagnostic.decode(errors="replace"), file=sys.stderr)
        assert process.returncode == 0, f"trading service exited with {process.returncode}"
print("Trading instance admission, bounded TLS handshakes and clean stop passed")
