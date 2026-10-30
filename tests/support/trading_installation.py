"""A trading service installation for tests, laid out as setup.sh installs it
(apps/services/trading/settings.hpp), with links to the built programs.

    install(root, programs) -> Installation
    installation.account(name, port, terminals) writes config/accounts/<name>.json
    installation.start(name) runs that account's instance
    installation.client(name) a TLS socket as the test Terminal, checked
        against this host's certificate fingerprint
"""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import ssl
import subprocess
import time

REPOSITORY = Path(__file__).resolve().parents[2]


@dataclass
class Programs:
    trading: str
    certificates: str
    ctp_trader: str


def identities(certificates, directory, *names):
    """Writes <name>.crt/.key for each name; returns {name: fingerprint}."""
    output = subprocess.run([certificates, str(directory), *names], check=True,
                            capture_output=True, text=True).stdout
    return dict(line.split() for line in output.splitlines())


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Installation:
    def __init__(self, root, programs):
        self.root = Path(root)
        for folder in ("bin", "lib", "config/tls", "config/accounts", "logs", "data"):
            (self.root / folder).mkdir(parents=True, exist_ok=True)
        def link(target, path):
            os.symlink(os.path.abspath(target), self.root / path)
        link(programs.trading, "bin/asterion_trading")
        link(programs.ctp_trader, "lib/ctp-trader.so")
        (self.root / "config/logging.json").write_bytes(subprocess.check_output(
            ["bash", "-c", 'source "$1"; logging_settings', "setup",
             str(REPOSITORY / "apps/services/trading/scripts/setup.sh")]))
        self.keys = self.root / "test-identities"
        self.keys.mkdir()
        found = identities(os.path.abspath(programs.certificates), self.keys, "server", "terminal", "stranger")
        for suffix in ("crt", "key"):
            (self.root / f"config/tls/server.{suffix}").write_bytes(
                (self.keys / f"server.{suffix}").read_bytes())
        self.server, self.terminal, self.stranger = (found[n] for n in ("server", "terminal", "stranger"))
        self.ports = {}

    def account(self, name, port=None, terminals=None, environment="simulation", user="000001"):
        self.ports[name] = port or free_port()
        (self.root / f"config/accounts/{name}.json").write_text(json.dumps({
            "bind": "127.0.0.1", "port": self.ports[name],
            "terminals": [self.terminal] if terminals is None else terminals,
            "environment": environment, "broker": {"front": "tcp://127.0.0.1:41205", "broker_id": "9999", "user_id": user, "app_id": "client_app"}}))
        credentials = self.root / "test-credentials" / name
        credentials.mkdir(parents=True, exist_ok=True)
        (credentials / "password").write_text("test-only-password")
        (credentials / "auth_code").write_text("test-only-auth")
        return self.ports[name]

    def start(self, name, **options):
        options["env"] = dict(options.get("env", os.environ),
                              CREDENTIALS_DIRECTORY=str(self.root / "test-credentials" / name))
        process = subprocess.Popen([str(self.root / "bin/asterion_trading"), "--account", name],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, **options)
        deadline = time.monotonic() + 10
        while True:
            assert process.poll() is None, process.stderr.read().decode(errors="replace")
            try:
                with socket.create_connection(("127.0.0.1", self.ports[name]), timeout=1):
                    return process
            except ConnectionRefusedError:
                assert time.monotonic() < deadline, "the trading instance did not listen"
                time.sleep(.05)

    def context(self, identity="terminal"):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        # The server is checked by its fingerprint below, not by a CA.
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(str(self.keys / f"{identity}.crt"), str(self.keys / f"{identity}.key"))
        return context

    def client(self, name, identity="terminal"):
        raw = socket.create_connection(("127.0.0.1", self.ports[name]), timeout=3)
        try:
            channel = self.context(identity).wrap_socket(raw)
        except BaseException:
            raw.close()
            raise
        presented = hashlib.sha256(channel.getpeercert(binary_form=True)).hexdigest()
        assert presented == self.server, "the trading host presented another certificate"
        return channel


def install(root, programs):
    return Installation(root, programs)
