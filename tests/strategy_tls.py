"""Exercise the real strategy process over mutually authenticated TCP."""
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

binary, certificates, protoc, proto_root = sys.argv[1:]


def protobuf(kind, content, decode=False):
    command = [protoc, f"--proto_path={proto_root}",
               f"--{'decode' if decode else 'encode'}=asterion.strategy.v1.{kind}",
               str(Path(proto_root) / "asterion/v1/strategy.proto")]
    return subprocess.run(command, input=content, capture_output=True, check=True).stdout


def request(operation):
    return protobuf("Request", ("version: 1 session_id: 'tls.strategy' "
                                "correlation_id: 'tls.request' " + operation).encode())


with tempfile.TemporaryDirectory(prefix="asterion-strategy-tls-") as folder:
    root = Path(folder)
    journal = root / "journal"
    journal.mkdir()
    subprocess.run([certificates, folder], check=True, capture_output=True)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    args = [binary, "--session", "tls.strategy", "--directory", str(journal),
            "--bind", "127.0.0.1", "--port", str(port), "--tls-ca", str(root / "ca.crt"),
            "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")]
    context = ssl.create_default_context(cafile=str(root / "ca.crt"))
    context.load_cert_chain(str(root / "client.crt"), str(root / "client.key"))

    def call(payload, ctx=context):
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            with ctx.wrap_socket(sock, server_hostname="localhost") as channel:
                channel.sendall(struct.pack("!I", len(payload)) + payload)

                def read(size):
                    data = b""
                    while len(data) < size:
                        block = channel.recv(size - len(data))
                        if not block:
                            raise ConnectionError("strategy closed connection")
                        data += block
                    return data
                size = struct.unpack("!I", read(4))[0]
                assert 0 < size <= 16 * 1024 * 1024
                return read(size)

    def start():
        process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            assert process.poll() is None, process.stderr.read()
            try:
                result = protobuf("Response", call(request("heartbeat {}")), decode=True)
                assert b"health {" in result, result
                return process
            except OSError:
                time.sleep(0.05)
        process.kill()
        process.communicate(timeout=10)
        raise AssertionError("strategy did not become reachable")

    def stop(process):
        process.kill()
        process.communicate(timeout=10)

    create = request('''create {
        version: 1 session_id: "tls.strategy" stream_id: "tls.market"
        plugin_id: "asterion.strategy.cta.sma-long-flat" fast: 1 slow: 2
        quantity { units: 100000000 }
        contract { venue: "SHFE" symbol: "rb2610" currency: "CNY"
            price_increment { units: 100000000 } quantity_increment { units: 100000000 }
            multiplier { units: 1000000000 } product: "rb" delivery_month: "2026-10" }
    }''')

    def event(sequence, price):
        return request(f'''event {{ stream_id: "tls.market" sequence: {sequence}
            tick {{ timestamp_ns: {sequence} price {{ units: {price * 100000000} }}
                quantity {{ units: 100000000 }} }} }}''')

    process = start()
    try:
        anonymous = ssl.create_default_context(cafile=str(root / "ca.crt"))
        try:
            call(create, anonymous)
            raise AssertionError("client without identity was accepted")
        except OSError:
            pass
        assert not list(journal.glob("*.json"))
        assert b"snapshot {" in protobuf("Response", call(create), decode=True)
        call(event(1, 100))
        result = call(event(2, 101))
        decoded = protobuf("Response", result, decode=True)
        assert b"intent {" in decoded and b"units: 100000000" in decoded, decoded
        assert result == call(event(2, 101))
        assert b"error {" in protobuf("Response", call(event(2, 999)), decode=True)
        assert len(list(journal.glob("*.json"))) == 3
    finally:
        stop(process)
    process = start()
    try:
        assert result == call(event(2, 101))
        decoded = protobuf("Response", call(event(3, 99)), decode=True)
        assert b"intent {" in decoded and b"target_quantity {" in decoded, decoded
        assert b"units:" not in decoded, decoded  # flat target uses protobuf's zero default
        assert len(list(journal.glob("*.json"))) == 4
    finally:
        stop(process)
print("Strategy TCP/mTLS identity, durable replay and duplicate/conflict handling verified")
