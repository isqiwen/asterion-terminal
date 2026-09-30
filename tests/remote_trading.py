from history_fixture import seed
"""Real TCP/mTLS, native Terminal client, daemon and durable server ledger."""
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

bridge, trading, certificates = sys.argv[1:]

def request(process, method, params=None, error=False):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    value = json.loads(process.stdout.readline())
    if error:
        assert "error" in value, value
        return value
    assert "result" in value, value
    return value["result"]

def launch():
    return subprocess.Popen([bridge], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")

def stop(process):
    if process.poll() is None:
        process.kill()
    process.communicate(timeout=10)

with tempfile.TemporaryDirectory(prefix="asterion-tcp-中文-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    subprocess.run([certificates, folder], check=True, timeout=20)
    account = root / "server-ledger"
    account.mkdir()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    args = [trading, "--mode", "paper", "--session", "paper.remote", "--bind", "0.0.0.0", "--port", str(port), "--directory", str(account),
            "--tls-ca", str(root / "ca.crt"), "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")]
    def server(wrong_identity=False):
        command = list(args)
        if wrong_identity:
            command[command.index("--tls-cert") + 1] = str(root / "wrong.crt")
            command[command.index("--tls-key") + 1] = str(root / "wrong.key")
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(200):
            assert process.poll() is None, process.stderr.read()
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    return process
            except OSError:
                time.sleep(0.01)
        raise AssertionError("server did not listen")
    params = {"host": "127.0.0.1", "port": str(port), "session": "paper.remote", "mode": "paper", "ca_file": str(root / "ca.crt"),
              "certificate_file": str(root / "client.crt"), "private_key_file": str(root / "client.key")}
    daemon, terminal = server(wrong_identity=True), launch()
    try:
        mismatch = request(terminal, "paper.connect", params, error=True)
        assert "certificate verify failed" in mismatch["error"]["message"], mismatch
        stop(daemon)
        daemon = server()
        for bad in ({"ca_file": str(root / "other.crt")},
                    {"certificate_file": str(root / "stranger.crt"), "private_key_file": str(root / "stranger.key")},
                    {"certificate_file": str(root / "expired.crt"), "private_key_file": str(root / "expired.key")},
                    {"session": "wrong-session"}, {"mode": "live"}, {"port": "123abc"}):
            request(terminal, "paper.connect", {**params, **bad}, error=True)
        assert not list(account.iterdir()), "rejected clients must not create a ledger"
        # Missing client identity is rejected even when the server identity is valid.
        ctx = ssl.create_default_context(cafile=params["ca_file"])
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock, ctx.wrap_socket(sock, server_hostname="localhost") as tls:
                tls.sendall(b"\x00\x00\x00\x01x")
                assert tls.recv(1) == b"", "unauthenticated peer received application data"
        except ssl.SSLError as error:
            assert "CERTIFICATE_VERIFY_FAILED" not in str(error), error
        connected = request(terminal, "paper.connect", params)
        assert connected["paper"] is None and connected["connection"]["state"] == "connected"
        source = root / "ticks.csv"
        source.write_text("timestamp_ns,price,quantity\n100,100,1\n200,99,1\n300,110,1\n", encoding="utf-8")
        seed(lambda method, params=None: request(terminal, method, params), [100,99,110], "fixture0")
        request(terminal, "paper.create", {"deposit": "1000", "margin_per_lot": "100", "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0", "close_yesterday_fee_rate": "0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        action = {"request_id": "remote.tick1", "action": "advance"}
        observer = launch()
        try:
            request(observer, "paper.connect", params)
            # Two persistent authenticated clients race the same durable command.
            # The account must commit once, while both clients stay connected.
            with ThreadPoolExecutor(max_workers=2) as peers:
                first = peers.submit(request, terminal, "paper.act", action)
                second = peers.submit(request, observer, "paper.act", action)
                expected = first.result()["paper"]
                assert second.result()["paper"] == expected
            assert request(observer, "runtime.snapshot")["paper"] == expected
        finally:
            stop(observer)
        assert expected["cursor"] == 1 and (account / "journal.sqlite").exists()
        request(terminal, "paper.close")
        assert daemon.poll() is None
        assert request(terminal, "paper.connect", params)["paper"] == expected
        assert request(terminal, "paper.reconnect")["paper"] == expected
        stop(terminal)
        assert daemon.poll() is None, "Terminal termination killed the service"
        terminal = launch()
        assert request(terminal, "paper.connect", params)["paper"] == expected
        stop(daemon)
        stale = request(terminal, "runtime.snapshot")
        assert stale["connection"]["state"] == "disconnected" and stale["paper"]["storage_state"] == "recovery_required"
        daemon = server()
        assert request(terminal, "paper.reconnect")["paper"] == expected
        assert request(terminal, "paper.act", action)["paper"] == expected, "duplicate command changed the ledger"
        request(terminal, "paper.close")
        # Fragmented Protobuf framing and remote shutdown denial.
        ctx.load_cert_chain(params["certificate_file"], params["private_key_file"])
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock, ctx.wrap_socket(sock, server_hostname="localhost") as tls:
            shutdown = b"\x08\x01\x12\x0cpaper.remote\x18\x01\x22\x01t\x72\x00"
            frame = struct.pack("!I", len(shutdown)) + shutdown
            for fragment in (frame[:1], frame[1:3], frame[3:8], frame[8:]):
                tls.sendall(fragment)
            def read_exact(size):
                data = b""
                while len(data) < size:
                    chunk = tls.recv(size - len(data))
                    assert chunk
                    data += chunk
                return data
            response = read_exact(struct.unpack("!I", read_exact(4))[0])
            assert b"Node Agent operation" in response
        assert daemon.poll() is None
        # Partial frames have a bounded lifetime even when the sender stays open.
        with socket.create_connection(("127.0.0.1", port), timeout=15) as sock, ctx.wrap_socket(sock, server_hostname="localhost") as tls:
            start = time.monotonic()
            tls.sendall(b"\x00")
            assert tls.recv(1) == b""
            assert time.monotonic() - start < 13
        # Reject an oversized frame, retain the service and committed state.
        ctx.load_cert_chain(params["certificate_file"], params["private_key_file"])
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock, ctx.wrap_socket(sock, server_hostname="localhost") as tls:
            tls.sendall(struct.pack("!I", 16 * 1024 * 1024 + 1))
            assert tls.recv(1) == b""
        assert request(terminal, "paper.connect", params)["paper"] == expected
    finally:
        stop(terminal)
        stop(daemon)
print("TCP/mTLS identity rejection, remote initialization, reconnect, independent lifecycle, crash recovery and command deduplication verified")
