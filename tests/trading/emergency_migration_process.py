"""Isolated process verification: emergency control protocol fallback, single-owner lock, and cross-host cold-standby migration."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [
    str(source / "tests/trading"),
    str(source / "tests/support"),
    str(source / "apps/services/common/python"),
    str(build / "generated-python"),
]

from asterion import (  # noqa: E402
    common_pb2 as common,
    emergency_pb2 as emergency,
    strategy_pb2 as wire,
    trading_pb2 as trading,
)
from strategy_process import (  # noqa: E402
    account_rpc,
    terminate,
    until,
)
from trading_installation import Programs, install  # noqa: E402

STRATEGY_CODE = """from asterion_strategy import Strategy

class HoldReady(Strategy):
    params = {}

    def ready(self, ctx):
        return True

    def on_bars(self, ctx, bars):
        return
"""


def write_atomic(path: Path, text: str) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(text)
    tmp.replace(path)


def provision_host(install_dir: Path):
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    host = install(
        install_dir,
        Programs(
            str(build / "asterion_trading"),
            str(build / "asterion_test_certificates"),
            str(build / f"libasterion_test_ctp_trader{suffix}"),
        ),
    )
    (host.root / "lib/ctp-md.so").symlink_to(build / f"libasterion_test_ctp{suffix}")
    code = host.root / "lib/strategy"
    (code / "asterion").mkdir(parents=True)
    shutil.copy2(source / "apps/services/trading/python/strategy_runner.py", code)
    shutil.copytree(
        source / "apps/services/common/python/asterion_strategy",
        code / "asterion_strategy",
        ignore=shutil.ignore_patterns("__pycache__"),
    )
    for name in ("strategy_pb2.py", "common_pb2.py"):
        shutil.copy2(build / "generated-python/asterion" / name, code / "asterion")
    (host.root / "runtime").mkdir()
    (host.root / "runtime/python").symlink_to(Path(sys.executable).resolve().parents[1])
    return host


def rpc_on(host, account_name: str, request: trading.Request, expected_error: bool = False) -> trading.Response:
    import struct

    request.id = 1
    with host.client(account_name) as channel:
        channel.settimeout(20)
        raw = request.SerializeToString()
        channel.sendall(struct.pack("!I", len(raw)) + raw)
        hdr = bytearray()
        while len(hdr) < 4:
            chunk = channel.recv(4 - len(hdr))
            assert chunk
            hdr.extend(chunk)
        (size,) = struct.unpack("!I", bytes(hdr))
        body = bytearray()
        while len(body) < size:
            chunk = channel.recv(size - len(body))
            assert chunk
            body.extend(chunk)
        reply = trading.Response.FromString(bytes(body))
    if expected_error:
        assert reply.HasField("error"), reply
    else:
        assert not reply.HasField("error"), reply
    return reply


def build_package() -> wire.Package:
    pkg = wire.Package(
        manifest=wire.Manifest(
            name="hold-ready",
            version="1",
            entry="hold_ready:HoldReady",
            runtime="python3.12-numpy2.3.5-scipy1.17.1",
            files=["hold_ready.py"],
        )
    )
    f = pkg.files.add()
    f.path = "hold_ready.py"
    f.content = STRATEGY_CODE.encode()
    return pkg


def make_warmup() -> wire.Warmup:
    import hashlib

    w = wire.Warmup(dataset_version="ver-20261117")
    digest = hashlib.sha256()
    batch = w.bars.add(sequence=1)
    digest.update(b"batch\n")
    batch.bars.add(
        instrument=common.Instrument(exchange="SHFE", symbol="rb2610"),
        trading_day="2026-09-25",
        end_ms=1758783600000,
        open=common.Decimal(text="3490"),
        high=common.Decimal(text="3505"),
        low=common.Decimal(text="3485"),
        close=common.Decimal(text="3500"),
        volume=1200,
        open_interest=common.Decimal(text="50000"),
    )
    digest.update(b"2026-09-25|1758783600000|SHFE/rb2610|3490|3505|3485|3500|1200|50000\n")
    w.sha256 = digest.hexdigest()
    return w


def main() -> None:
    secret_path = "/hook/migration-903"
    received: list[dict] = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode()
            assert self.path == secret_path, self.path
            with lock:
                received.append(json.loads(raw))
            self.send_response(200)
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    webhook_url = f"http://127.0.0.1:{server.server_port}{secret_path}"

    with tempfile.TemporaryDirectory(prefix="ast-emg-mig-") as folder:
        root = Path(folder)
        install_dir = root / "opt/asterion/trading"
        install_dir.parent.mkdir(parents=True)

        # --- Host A setup ---
        host_a = provision_host(install_dir)
        host_a.account("account", user="000001")
        host_a.account("alias", user="000001")
        for name in ("account", "alias"):
            (host_a.root / f"test-credentials/{name}/asterion.trading.{name}.webhook").write_text(
                webhook_url
            )
        flow = host_a.root / "data/account/ctp-flow"
        flow.mkdir(parents=True, exist_ok=True)
        write_atomic(flow / "counter-state", "20260928 1000000 1000000 0 0 50000 0\n")

        proc_a = host_a.start("account")
        proc_alias = None
        proc_b = None
        proc_c = None
        try:
            policy = trading.Policy(
                limits=dict(
                    max_order_lots={"text": "5"},
                    max_gross_lots={"text": "20"},
                    max_contract_lots={"text": "20"},
                    max_working_orders=4,
                ),
                max_price_deviation={"text": "0.02"},
            )
            policy.contracts.add(
                instrument=dict(exchange="SHFE", symbol="rb2610"),
                product="rb",
                delivery_month="2026-10",
                multiplier={"text": "10"},
                price_tick={"text": "1"},
                lot_size={"text": "1"},
            )
            account_rpc(host_a, trading.Request(open_account=policy))
            health_a = until(
                lambda: account_rpc(host_a, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            assert health_a.protocol and health_a.emergency_protocol
            assert health_a.protocol != health_a.emergency_protocol

            acct_a = account_rpc(host_a, trading.Request(get_account={})).get_account
            rev = acct_a.session.policy_revision

            def send_cmd(host, cmd: trading.Command) -> trading.Response:
                cmd.account_id = "account"
                cmd.policy_revision = rev
                return rpc_on(host, "account", trading.Request(send_command=cmd))

            # 1) Single-owner lock:
            # 1a) A duplicate process for the same account instance cannot acquire the journal lease / port.
            dup_env = dict(os.environ, CREDENTIALS_DIRECTORY=str(host_a.root / "test-credentials/account"))
            dup = subprocess.run(
                [str(host_a.root / "bin/asterion_trading"), "--account", "account"],
                env=dup_env,
                capture_output=True,
                text=True,
                timeout=10,
            )
            assert dup.returncode != 0, "duplicate account owner unexpectedly succeeded"

            # 1b) Another instance ("alias") configured with the same CTP (broker_id, user_id) is refused by owners/ FileLock!
            proc_alias = host_a.start("alias")
            alias_err = rpc_on(
                host_a, "alias", trading.Request(open_account=policy), expected_error=True
            ).error
            assert alias_err.code == common.ERROR_CODE_CONFLICT, alias_err
            assert "account execution owner is already active" in alias_err.message, alias_err
            terminate(proc_alias)
            proc_alias = None

            # 2) Emergency control contract verification (independent of routine trading_protocol):
            send_cmd(
                host_a,
                trading.Command(
                    request_id="auth.a1", authorize=trading.Authorize(user_id="000001")
                ),
            )
            # Place a 3-lot resting working order on SHFE.rb2610.
            send_cmd(
                host_a,
                trading.Command(
                    request_id="open.rest.emg",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-emg-rest",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "3"},
                        price={"text": "3500"},
                    ),
                ),
            )
            until(
                lambda: account_rpc(host_a, trading.Request(get_account={})).get_account.session,
                lambda s: any(o.id == "ord-emg-rest" for o in s.working_orders),
                seconds=12,
            )

            # Publish and start a live Python strategy instance ("inst-emg").
            pkg = build_package()
            account_rpc(host_a, trading.Request(publish_strategy=pkg))
            inst_cfg = wire.Instance(
                id="inst-emg",
                package_name=pkg.manifest.name,
                package_version=pkg.manifest.version,
                period=common.PERIOD_DAY,
                warmup=make_warmup(),
            )
            inst_cfg.instruments.add(exchange="SHFE", symbol="rb2610")
            account_rpc(host_a, trading.Request(start_strategy=inst_cfg))
            until(
                lambda: account_rpc(host_a, trading.Request(get_strategies={})).get_strategies.instance,
                lambda st: st.state == trading.INSTANCE_STATE_READY and st.process_id > 0,
                seconds=15,
            )

            # Inspect emergency state via frozen emergency.proto contract.
            emg = account_rpc(host_a, trading.Request(get_emergency={})).get_emergency
            assert emg.protocol == health_a.emergency_protocol
            assert emg.account_id == "account"
            assert emg.policy_revision == rev
            assert emg.account_open and emg.broker_ready and emg.authorized
            assert emg.active_strategy_id == "inst-emg"
            assert [o.id for o in emg.working_orders] == ["ord-emg-rest"]

            # Execute emergency stop_strategy, cancel_order, and revoke!
            emg = account_rpc(
                host_a,
                trading.Request(
                    emergency_control=emergency.Control(
                        stop_strategy=emergency.StopStrategy(instance_id="inst-emg")
                    )
                ),
            ).emergency_control
            assert emg.active_strategy_id == "", emg

            emg = account_rpc(
                host_a,
                trading.Request(
                    emergency_control=emergency.Control(
                        cancel_order=emergency.CancelOrder(
                            request_id="emg.cancel.1",
                            account_id=emg.account_id,
                            policy_revision=emg.policy_revision,
                            order_id="ord-emg-rest",
                        )
                    )
                ),
            ).emergency_control
            until(
                lambda: account_rpc(host_a, trading.Request(get_emergency={})).get_emergency,
                lambda st: len(st.working_orders) == 0,
                seconds=12,
            )

            emg = account_rpc(
                host_a,
                trading.Request(
                    emergency_control=emergency.Control(
                        revoke=emergency.Revoke(
                            request_id="emg.revoke.1",
                            account_id=emg.account_id,
                            policy_revision=emg.policy_revision,
                        )
                    )
                ),
            ).emergency_control
            assert not emg.authorized, emg

            # 3) Cold-standby host migration drill (Host A -> Host B):
            # Re-authorize on Host A, open a 2-lot position, and run strategy "inst-mig".
            send_cmd(
                host_a,
                trading.Command(
                    request_id="auth.a2", authorize=trading.Authorize(user_id="000001")
                ),
            )
            send_cmd(
                host_a,
                trading.Command(
                    request_id="open.pos.a",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-pos-a",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "2"},
                        price={"text": "3500"},
                    ),
                ),
            )
            until(
                lambda: account_rpc(host_a, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 1 and s.positions_reconciled,
                seconds=12,
            )
            inst_mig = wire.Instance(
                id="inst-mig",
                package_name=pkg.manifest.name,
                package_version=pkg.manifest.version,
                period=common.PERIOD_DAY,
                warmup=make_warmup(),
            )
            inst_mig.instruments.add(exchange="SHFE", symbol="rb2610")
            account_rpc(host_a, trading.Request(start_strategy=inst_mig))
            st_mig = until(
                lambda: account_rpc(host_a, trading.Request(get_strategies={})).get_strategies.instance,
                lambda st: st.state == trading.INSTANCE_STATE_READY and st.process_id > 0,
                seconds=15,
            )
            child_pid_a = st_mig.process_id

            # Persist counter state (2 lots carried) for the new host's counter connection.
            write_atomic(flow / "counter-state", "20260928 1000000 1000000 0 0 7000 2\n")

            # Step 3a: Stop Host A cleanly and verify both Trading and strategy child processes exited.
            terminate(proc_a)
            proc_a = None
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    os.kill(child_pid_a, 0)
                    time.sleep(0.05)
                except ProcessLookupError:
                    break
            else:
                raise AssertionError("Host A strategy child process still alive after shutdown")

            # Step 3b: Cold backup of data/ (owners/, account/ journal, archives, strategies),
            # decommission Host A completely, and provision Host B with fresh TLS server keys & credentials.
            backup_data = root / "cold_backup_data"
            shutil.copytree(install_dir / "data", backup_data, symlinks=True)
            shutil.rmtree(install_dir)

            host_b = provision_host(install_dir)
            assert host_b.server != host_a.server, "Host B must have its own TLS server identity"
            shutil.rmtree(host_b.root / "data")
            shutil.copytree(backup_data, host_b.root / "data", symlinks=True)
            host_b.account("account", user="000001")
            # Update front & app_id on Host B (allowed by deployment.md while keeping broker_id & user_id).
            cfg_path = host_b.root / "config/accounts/account.json"
            cfg = json.loads(cfg_path.read_text())
            cfg["broker"]["front"] = "tcp://127.0.0.1:41215"
            cfg["broker"]["app_id"] = "client_app_host_b"
            cfg_path.write_text(json.dumps(cfg))
            (host_b.root / "test-credentials/account/asterion.trading.account.webhook").write_text(
                webhook_url
            )

            # Step 3c: Start Host B, verify reconciliation, cleared authorization, preserved strategy run, and resume!
            proc_b = host_b.start("account")
            until(
                lambda: account_rpc(host_b, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            sess_b = until(
                lambda: account_rpc(host_b, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 1 and s.positions_reconciled,
                seconds=15,
            )
            assert not sess_b.HasField("authorized_ms"), "Host B takeover must never restore authorization"
            assert sess_b.policy_revision == rev
            assert sess_b.positions[0].yesterday_lots.text == "2"

            strats_b = account_rpc(host_b, trading.Request(get_strategies={})).get_strategies
            assert len(strats_b.packages) == 1
            run_b = account_rpc(
                host_b, trading.Request(get_strategy_run={"id": "inst-mig"})
            ).get_strategy_run
            assert run_b.configuration.id == "inst-mig"

            # Re-authorize on Host B, close migrated position, then resume a new strategy instance from inst-mig!
            send_cmd(
                host_b,
                trading.Command(
                    request_id="auth.b1", authorize=trading.Authorize(user_id="000001")
                ),
            )
            send_cmd(
                host_b,
                trading.Command(
                    request_id="close.pos.b",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-close-b",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_SELL,
                        offset=common.OFFSET_CLOSE_YESTERDAY,
                        lots={"text": "2"},
                        price={"text": "3500"},
                    ),
                ),
            )
            until(
                lambda: account_rpc(host_b, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 0 and s.positions_reconciled,
                seconds=12,
            )

            inst_b = wire.Instance(
                id="inst-host-b",
                package_name=pkg.manifest.name,
                package_version=pkg.manifest.version,
                period=common.PERIOD_DAY,
                resume_from="inst-mig",
            )
            inst_b.instruments.add(exchange="SHFE", symbol="rb2610")
            account_rpc(host_b, trading.Request(start_strategy=inst_b))
            st_b = until(
                lambda: account_rpc(host_b, trading.Request(get_strategies={})).get_strategies.instance,
                lambda st: st.state == trading.INSTANCE_STATE_READY and st.process_id > 0,
                seconds=15,
            )
            child_pid_b = st_b.process_id

            # 4) Unclean crash failover drill (Host B killed with SIGKILL -> Host C takeover):
            write_atomic(flow / "counter-state", "20260928 1000000 1000000 0 0 0 0\n")
            proc_b.send_signal(signal.SIGKILL)
            proc_b.wait(timeout=5)
            proc_b = None
            try:
                os.kill(child_pid_b, signal.SIGKILL)
            except ProcessLookupError:
                pass

            crash_backup = root / "crash_backup_data"
            shutil.copytree(install_dir / "data", crash_backup, symlinks=True)
            shutil.rmtree(install_dir)

            host_c = provision_host(install_dir)
            shutil.rmtree(host_c.root / "data")
            shutil.copytree(crash_backup, host_c.root / "data", symlinks=True)
            host_c.account("account", user="000001")
            (host_c.root / "test-credentials/account/asterion.trading.account.webhook").write_text(
                webhook_url
            )

            proc_c = host_c.start("account")
            until(
                lambda: account_rpc(host_c, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            sess_c = account_rpc(host_c, trading.Request(get_account={})).get_account.session
            assert not sess_c.HasField("authorized_ms")
            strats_c = until(
                lambda: account_rpc(host_c, trading.Request(get_strategies={})).get_strategies.instance,
                lambda st: st.state == trading.INSTANCE_STATE_INTERRUPTED,
                seconds=10,
            )
            assert strats_c.instance.id == "inst-host-b"
            until(
                lambda: [
                    item
                    for item in received
                    if item.get("kind") == "strategy_failed"
                    and item.get("details", {}).get("state") == "interrupted"
                ],
                lambda items: len(items) == 1,
                seconds=5,
            )
        finally:
            for proc in (proc_a, proc_alias, proc_b, proc_c):
                if proc is not None:
                    terminate(proc)
            server.shutdown()


if __name__ == "__main__":
    main()
