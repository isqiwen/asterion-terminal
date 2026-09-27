"""Certificate roles authorize operations, not merely connections.

The node CA issues admin, client (business-only) and service certificates at
enrollment. A client certificate may read Agent status but cannot upload or
run programs; a certificate without a role is refused by every service.
"""
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

agent, trading, certificates = sys.argv[1:]


def varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number, data):
    return varint(number << 3 | 2) + varint(len(data)) + data


def decode(data):
    result, offset = {}, 0

    def integer():
        nonlocal offset
        value = shift = 0
        while True:
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7

    while offset < len(data):
        key = integer()
        number, wire = key >> 3, key & 7
        if wire == 0:
            result[number] = integer()
        else:
            size = integer()
            result[number] = data[offset:offset + size]
            offset += size
    return result


def exchange(port, root, name, payload):
    context = ssl.create_default_context(cafile=str(root / "ca.crt"))
    context.load_cert_chain(str(root / f"{name}.crt"), str(root / f"{name}.key"))
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
        with context.wrap_socket(raw, server_hostname="localhost") as channel:
            channel.sendall(struct.pack("!I", len(payload)) + payload)
            header = b""
            try:
                while len(header) < 4:
                    part = channel.recv(4 - len(header))
                    if not part:
                        return None  # closed without a reply
                    header += part
            except (ssl.SSLError, ConnectionError):
                return None
            size = struct.unpack("!I", header)[0]
            body = b""
            while len(body) < size:
                part = channel.recv(size - len(body))
                assert part, "reply truncated"
                body += part
            return decode(body)


def start(command, port):
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 10
    while True:
        assert process.poll() is None, process.stderr.read().decode(errors="replace")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return process
        except OSError:
            assert time.monotonic() < deadline, "service never listened"
            time.sleep(0.05)


def free_port():
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        return reserve.getsockname()[1]


with tempfile.TemporaryDirectory(prefix="ast-roles-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    (root / "agent").mkdir()
    (root / "ledger").mkdir()
    subprocess.run([certificates, folder], check=True)
    tls = ["--tls-ca", str(root / "ca.crt"), "--tls-cert", str(root / "server.crt"),
           "--tls-key", str(root / "server.key")]
    agent_port, trading_port = free_port(), free_port()
    processes = [
        start([agent, "--directory", str(root / "agent"), "--bind", "127.0.0.1",
               "--port", str(agent_port), *tls], agent_port),
        start([trading, "--mode", "paper", "--session", "roles", "--directory",
               str(root / "ledger"), "--bind", "127.0.0.1", "--port", str(trading_port), *tls],
              trading_port),
    ]
    try:
        status = varint(1 << 3) + varint(1) + field(2, b"status") + field(10, b"")
        upload = (varint(1 << 3) + varint(1) + field(2, b"upload") +
                  field(11, field(1, b"a" * 64) + varint(2 << 3) + varint(1) +
                        field(3, b"linux") + field(4, b"x86_64")))
        # Business-only certificate: status yes, installing programs no.
        reply = exchange(agent_port, root, "trading-client", status)
        assert reply and 10 in reply, reply
        reply = exchange(agent_port, root, "trading-client", upload)
        assert reply and 12 in reply, reply
        error = decode(reply[12])
        assert error.get(2) == b"permission_denied", error
        # The administrator certificate reaches the upload validation itself.
        reply = exchange(agent_port, root, "client", upload)
        assert reply and 12 in reply and decode(reply[12]).get(2) != b"permission_denied", reply
        # No role: authenticated by the CA but authorized for nothing.
        assert exchange(agent_port, root, "unroled", status) is None
        # Trading service: heartbeat for a client certificate, nothing for none.
        heartbeat = (varint(1 << 3) + varint(1) + field(2, b"roles") + varint(3 << 3) +
                     varint(1) + field(4, b"hb") + field(16, b""))
        reply = exchange(trading_port, root, "trading-client", heartbeat)
        assert reply and 14 in reply, reply
        assert exchange(trading_port, root, "unroled", heartbeat) is None
    finally:
        for process in processes:
            process.kill()
            process.wait(timeout=10)
print("Certificate roles gate Agent mutations and service admission")
