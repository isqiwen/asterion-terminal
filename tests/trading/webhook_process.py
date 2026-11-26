"""Headless Trading alert webhook delivery, deduplication, non-blocking I/O and secret redaction."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import sys
import tempfile
import threading
import time

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [
    str(source / "tests/support"),
    str(source / "apps/services/lab"),
    str(source / "apps/services/common/python"),
    str(build / "generated-python"),
]
from asterion import common_pb2 as common, strategy_pb2 as strategy, trading_pb2 as trading
from asterion_lab.package import build as build_package
from trading_installation import Programs, install


def account_rpc(host, request, expected_error=False):
    request.id = 1
    with host.client("account") as channel:
        channel.settimeout(20)
        raw = request.SerializeToString()
        channel.sendall(struct.pack("!I", len(raw)) + raw)

        def exact(size):
            result = bytearray()
            while len(result) < size:
                chunk = channel.recv(size - len(result))
                assert chunk, "Trading closed its reply"
                result.extend(chunk)
            return bytes(result)

        (size,) = struct.unpack("!I", exact(4))
        response = trading.Response.FromString(exact(size))
    if expected_error:
        assert response.HasField("error"), response
    else:
        assert not response.HasField("error"), response
    return response


def until(operation, predicate, seconds=12):
    end = time.monotonic() + seconds
    while True:
        value = operation()
        if predicate(value):
            return value
        assert time.monotonic() < end, value
        time.sleep(0.05)


def main():
    secret_token = "secret-webhook-token-991"
    secret_path = f"/hook/{secret_token}"
    received = []
    lock = threading.Lock()
    mode = {"behavior": "ok"}

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode()
            assert self.path == secret_path, self.path
            behavior = mode["behavior"]
            if behavior == "stall":
                time.sleep(4.0)
                self.send_response(503)
                self.end_headers()
                return
            if behavior == "fail_once":
                mode["behavior"] = "ok"
                self.send_response(503)
                self.end_headers()
                return
            with lock:
                received.append(json.loads(raw))
            self.send_response(500 if behavior == "fail" else 200)
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    webhook_url = f"http://127.0.0.1:{server.server_port}{secret_path}"

    with tempfile.TemporaryDirectory(prefix="ast-webhook-test-") as folder:
        root = Path(folder)
        suffix = ".dylib" if sys.platform == "darwin" else ".so"
        host = install(
            root / "trading",
            Programs(
                str(build / "asterion_trading"),
                str(build / "asterion_test_certificates"),
                str(build / f"libasterion_test_ctp_trader{suffix}"),
            ),
        )
        code = host.root / "lib/strategy"
        (host.root / "lib/ctp-md.so").symlink_to(build / f"libasterion_test_ctp{suffix}")
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
        (host.root / "runtime/python").symlink_to(
            Path(sys.executable).resolve().parents[1], target_is_directory=True
        )

        host.account("account", user="webhook")
        (host.root / "test-credentials/account/asterion.trading.account.webhook").write_text(
            webhook_url
        )
        process = host.start("account")
        try:
            policy = trading.Policy(
                limits=dict(
                    max_order_lots={"text": "5"},
                    max_gross_lots={"text": "10"},
                    max_contract_lots={"text": "10"},
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
            account_rpc(host, trading.Request(open_account=policy))
            until(
                lambda: account_rpc(host, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            current = account_rpc(host, trading.Request(get_account={})).get_account
            rev = current.session.policy_revision

            def send_cmd(cmd):
                cmd.account_id = current.id
                cmd.policy_revision = rev
                return account_rpc(host, trading.Request(send_command=cmd))

            send_cmd(
                trading.Command(
                    request_id="auth.1", authorize=trading.Authorize(user_id="webhook")
                )
            )

            # 1. Slow/failing webhook must never block order submission or cancellation.
            reject_flag = host.root / "data/account/ctp-flow/reject"
            mode["behavior"] = "stall"
            started = time.monotonic()
            reject_flag.touch()
            send_cmd(
                trading.Command(
                    request_id="sub.rej",
                    submit_order=trading.SubmitOrder(
                        order_id="rejected.1",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                )
            )
            reject_flag.unlink()
            time.sleep(1.05)  # CTP 1s quote spacing
            send_cmd(
                trading.Command(
                    request_id="sub.rest",
                    submit_order=trading.SubmitOrder(
                        order_id="resting.1",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "3"},
                        price={"text": "3500"},
                    ),
                )
            )
            until(
                lambda: account_rpc(host, trading.Request(get_orders={})).get_orders.orders,
                lambda rows: any(r.id == "resting.1" and r.state == trading.ORDER_STATE_ACCEPTED for r in rows),
            )
            send_cmd(
                trading.Command(
                    request_id="cancel.rest",
                    cancel_order=trading.OrderId(id="resting.1"),
                )
            )
            elapsed = time.monotonic() - started
            assert elapsed < 3.5, f"stalled webhook blocked trading path: {elapsed:.2f}s"

            mode["behavior"] = "fail_once"
            # Trigger a second rejected order while webhook receiver transiently returns 503; retry must deliver it.
            time.sleep(1.05)
            reject_flag.touch()
            send_cmd(
                trading.Command(
                    request_id="sub.rej2",
                    submit_order=trading.SubmitOrder(
                        order_id="rejected.2",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                )
            )
            reject_flag.unlink()
            until(
                lambda: [item for item in received if item["kind"] == "order_rejected"],
                lambda items: any(i["details"].get("order_id") == "rejected.2" for i in items),
            )

            # 2. Publish & start a strategy, then close all Terminal connections and crash the child.
            repo = root / "repo"
            repo.mkdir()
            (repo / "strategies").mkdir()
            (repo / "strategies/headless.py").write_text(
                "from asterion_strategy import Strategy\n"
                "class Headless(Strategy):\n"
                "    def ready(self, ctx): return True\n"
                "    def on_bar(self, ctx, bar): pass\n"
            )
            pkg = build_package(repo, "headless.Headless")
            account_rpc(host, trading.Request(publish_strategy=pkg))
            cfg = strategy.Instance(
                id="headless-run",
                package_name=pkg.manifest.name,
                package_version=pkg.manifest.version,
                period=common.PERIOD_MINUTES_1,
                instruments=[dict(exchange="SHFE", symbol="rb2610")],
            )
            running = account_rpc(host, trading.Request(start_strategy=cfg)).start_strategy.instance
            assert running.process_id > 0

            # No client connects during the following faults:
            os.kill(running.process_id, signal.SIGKILL)
            until(
                lambda: [item for item in received if item["kind"] == "strategy_failed"],
                lambda items: len(items) == 1 and items[0]["details"]["instance_id"] == "headless-run",
            )

            # 3. Headless broker disconnect -> deduplicated once -> reconnect -> second disconnect alerts again.
            disconnect_flag = host.root / "data/account/ctp-flow/disconnect"
            disconnect_flag.touch()
            until(
                lambda: [item for item in received if item["kind"] == "broker_disconnected"],
                lambda items: len(items) == 1,
            )
            time.sleep(0.3)
            assert len([item for item in received if item["kind"] == "broker_disconnected"]) == 1
            disconnect_flag.unlink()
            # Wait for headless reconnect + reconciliation, then drop a second time.
            until(
                lambda: account_rpc(host, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            disconnect_flag.touch()
            until(
                lambda: [item for item in received if item["kind"] == "broker_disconnected"],
                lambda items: len(items) == 2,
            )
            disconnect_flag.unlink()
            until(
                lambda: account_rpc(host, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )

            # 4. Submit a resting order, restart Trading while counter loses in-memory orders,
            # and verify headless unconfirmed_order alert with zero client ever connecting.
            current = account_rpc(host, trading.Request(get_account={})).get_account
            rev = current.session.policy_revision
            send_cmd(
                trading.Command(
                    request_id="auth.2", authorize=trading.Authorize(user_id="webhook")
                )
            )
            time.sleep(1.05)
            send_cmd(
                trading.Command(
                    request_id="sub.unconf",
                    submit_order=trading.SubmitOrder(
                        order_id="unconfirmed.1",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "3"},
                        price={"text": "3500"},
                    ),
                )
            )
            process.kill()
            process.communicate(timeout=10)
            process = host.start("account")
            # Do NOT connect any Terminal client; Trading must push unconfirmed_order on its own.
            unconfirmed_alerts = until(
                lambda: [item for item in received if item["kind"] == "unconfirmed_order"],
                lambda items: len(items) == 1,
                seconds=15,
            )
            assert unconfirmed_alerts[0]["details"]["order_id"] == "unconfirmed.1"
            time.sleep(0.3)
            assert len([item for item in received if item["kind"] == "unconfirmed_order"]) == 1
            # Only the restart with a trading record reports the ended authorization.
            assert len([item for item in received if item["kind"] == "service_restarted"]) == 1

            for item in received:
                assert item["service"] == "asterion-trading"
                assert item["account"] == "account"
                assert item["time_ms"] > 0 and item["text"].startswith("[Asterion account]")
        finally:
            if process and process.poll() is None:
                process.terminate()
                process.communicate(timeout=15)
            server.shutdown()

        # Secret webhook URL must never appear in logs, configs or records.
        for path in host.root.rglob("*"):
            if path.is_file() and "test-credentials" not in path.parts:
                assert secret_token.encode() not in path.read_bytes(), path


if __name__ == "__main__":
    main()
