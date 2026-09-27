"""An unauthenticated TCP peer must not stall the trading listener.

The first connection never starts TLS. Before handshakes moved into the worker
pool, the listener blocked on it for 10 s and a legitimate client timed out.
"""
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import os

# Sanitizer builds run several times slower.
SCALE = float(os.environ.get("ASTERION_TIMING_SCALE", "1"))

executable, certificates = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="ast-trading-admission-") as folder:
    root = Path(folder)
    (root / "ledger").mkdir()
    subprocess.run([certificates, folder], check=True)
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    process = subprocess.Popen([
        executable, "--mode", "paper", "--session", "adm", "--directory", str(root / "ledger"),
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
        with silent:
            time.sleep(.3)
            context = ssl.create_default_context(cafile=str(root / "ca.crt"))
            context.load_cert_chain(str(root / "client.crt"), str(root / "client.key"))
            context.minimum_version = ssl.TLSVersion.TLSv1_3
            started = time.monotonic()
            raw = socket.create_connection(("127.0.0.1", port), timeout=3)
            with context.wrap_socket(raw, server_hostname="localhost") as channel:
                # Request(version=1, session_id=adm, mode=PAPER, correlation_id=hb1, heartbeat={}).
                request = b"\x08\x01\x12\x03adm\x18\x01\x22\x03hb1\x82\x01\x00"
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
                assert response.startswith(b"\x08\x01\x12\x03adm\x18\x01\x22\x03hb1\x72"), response
            elapsed = time.monotonic() - started
            assert elapsed < 3 * SCALE, f"authenticated client waited {elapsed:.1f}s behind a silent peer"
    finally:
        process.kill()
        _, diagnostic = process.communicate(timeout=10)
        if diagnostic:
            print(diagnostic.decode(errors="replace"), file=sys.stderr)
print("Trading service answered an authenticated client while a silent TCP peer was pending")
