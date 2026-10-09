"""TLS admission is bounded, preserves existing business capacity and recovers."""
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

# Sanitizer builds run several times slower.
SCALE = float(os.environ.get("ASTERION_TIMING_SCALE", "1"))

executable, certificates, trader_sdk = sys.argv[1:]


def heartbeat(channel, correlation):
    assert len(correlation) == 3
    # Request(version=1, session_id=adm, correlation_id, heartbeat={}).
    prefix = b"\x08\x01\x12\x03adm\x22\x03" + correlation
    request = prefix + b"\x82\x01\x00"
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
    assert response.startswith(prefix + b"\x72"), response


def authenticated_client(context, port):
    # Closing silent peers is observed on the host's next admission poll. Only
    # failed handshakes are retried here; no business request has been sent.
    deadline = time.monotonic() + 3 * SCALE
    while True:
        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        try:
            return context.wrap_socket(raw, server_hostname="localhost")
        except (ssl.SSLError, ConnectionError, TimeoutError):
            raw.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(.05)


with tempfile.TemporaryDirectory(prefix="ast-trading-admission-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    (root / "ledger").mkdir()
    subprocess.run([certificates, folder], check=True)
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    process = subprocess.Popen([
        executable, "--session", "adm", "--directory", str(root / "ledger"),
        "--ctp-library", trader_sdk,
        "--bind", "127.0.0.1", "--port", str(port),
        "--tls-ca", str(root / "ca.crt"),
        "--tls-cert", str(root / "server.crt"),
        "--tls-key", str(root / "server.key"),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while True:
            assert process.poll() is None
            try:
                silent = socket.create_connection(("127.0.0.1", port), timeout=3)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.05)
        with ExitStack() as peers:
            peers.enter_context(silent)
            for _ in range(7):
                peers.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            time.sleep(.3)
            context = ssl.create_default_context(cafile=str(root / "ca.crt"))
            context.load_cert_chain(str(root / "client.crt"), str(root / "client.key"))
            context.minimum_version = ssl.TLSVersion.TLSv1_3
            started = time.monotonic()
            raw = socket.create_connection(("127.0.0.1", port), timeout=3)
            with context.wrap_socket(raw, server_hostname="localhost") as channel:
                heartbeat(channel, b"hb1")
            elapsed = time.monotonic() - started
            assert elapsed < 3 * SCALE, f"authenticated client waited {elapsed:.1f}s behind eight silent peers"
        print("PASS: eight silent peers cannot consume the business-worker pool", flush=True)

        for cycle in range(2):
            with authenticated_client(context, port) as business:
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
                with authenticated_client(context, port) as recovered:
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
print("Trading TLS admission and graceful resource cleanup passed")
