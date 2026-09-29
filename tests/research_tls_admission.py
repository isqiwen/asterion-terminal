"""Task TLS authentication gets an operation deadline, not the listener poll interval."""
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

executable, certificates = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="ast-research-admission-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    (root / "tasks").mkdir()
    subprocess.run([certificates, folder], check=True)
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    process = subprocess.Popen([
        executable, "--directory", str(root / "tasks"), "--session", "admission",
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
                raw = socket.create_connection(("127.0.0.1", port), timeout=3)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.05)
        with raw:
            # Deliberately longer than the 200 ms listener polling interval.
            time.sleep(.6)
            context = ssl.create_default_context(cafile=str(root / "ca.crt"))
            context.load_cert_chain(str(root / "client.crt"), str(root / "client.key"))
            context.minimum_version = ssl.TLSVersion.TLSv1_3
            with context.wrap_socket(raw, server_hostname="localhost") as channel:
                # TaskRequest(version=1, service_id=admission,
                # correlation_id=delayed, heartbeat={}).
                request = b"\x08\x01\x12\x09admission\x1a\x07delayed\x82\x01\x00"
                channel.sendall(struct.pack("!I", len(request)) + request)
                def receive(size):
                    data = b""
                    while len(data) < size:
                        part = channel.recv(size - len(data))
                        assert part, "task service closed before heartbeat response"
                        data += part
                    return data
                size = struct.unpack("!I", receive(4))[0]
                assert 0 < size <= 4096
                response = receive(size)
                assert response.startswith(b"\x08\x01\x12\x09admission\x1a\x07delayed\x6a"), response
        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            with context.wrap_socket(raw, server_hostname="localhost") as channel:
                # Authenticated public clients must not request worker dispatch.
                request = b"\x08\x01\x12\x09admission\x1a\x07blocked\xb2\x01\x00"
                channel.sendall(struct.pack("!I", len(request)) + request)
                size = struct.unpack("!I", receive(4))[0]
                assert 0 < size <= 4096
                response = receive(size)
                assert b"task dispatch requires the private worker channel" in response, response

    finally:
        process.kill()
        _, diagnostic = process.communicate(timeout=10)
        if diagnostic:
            print(diagnostic.decode(errors="replace"), file=sys.stderr)
print("Task service accepted delayed authenticated handshake and returned correlated health")
