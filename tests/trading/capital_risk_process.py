"""Isolated counter process verification: capital & cancel hard risk, cashflows, webhook alerts, rollover, and restart."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [
    str(source / "tests/trading"),
    str(source / "tests/support"),
    str(build / "generated-python"),
]

from asterion import (  # noqa: E402
    common_pb2 as common,
    trading_pb2 as trading,
)
from strategy_process import (  # noqa: E402
    account_rpc,
    terminate,
    until,
)
from trading_installation import Programs, install  # noqa: E402


def write_atomic(path: Path, text: str) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(text)
    tmp.replace(path)


def main() -> None:
    secret_path = "/hook/capital-risk-901"
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

    def alerts_for(reason: str) -> list[dict]:
        with lock:
            return [
                item
                for item in received
                if item.get("kind") == "risk_limit_breached"
                and item.get("details", {}).get("reason") == reason
            ]

    with tempfile.TemporaryDirectory(prefix="ast-cap-risk-") as folder:
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

        host.account("account", user="000001")
        (host.root / "test-credentials/account/asterion.trading.account.webhook").write_text(
            webhook_url
        )
        flow = host.root / "data/account/ctp-flow"
        flow.mkdir(parents=True, exist_ok=True)
        write_atomic(flow / "counter-state", "20260928 1000000 1000000 0 0 50000 0\n")

        process = host.start("account")
        try:
            policy = trading.Policy(
                limits=dict(
                    max_order_lots={"text": "5"},
                    max_gross_lots={"text": "20"},
                    max_contract_lots={"text": "20"},
                    max_working_orders=4,
                ),
                max_price_deviation={"text": "0.02"},
                max_daily_loss={"text": "15000"},
                max_drawdown={"text": "25000"},
                max_margin_ratio={"text": "0.7"},
                max_daily_cancels_per_contract=2,
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
            acct = account_rpc(host, trading.Request(get_account={})).get_account
            rev = acct.session.policy_revision

            def send_cmd(cmd: trading.Command, expected_error: bool = False) -> trading.Response:
                cmd.account_id = acct.id
                cmd.policy_revision = rev
                req = trading.Request(send_command=cmd)
                req.id = 1
                with host.client("account") as channel:
                    import struct

                    channel.settimeout(20)
                    raw = req.SerializeToString()
                    channel.sendall(struct.pack("!I", len(raw)) + raw)
                    buf = bytearray()
                    while len(buf) < 4:
                        chunk = channel.recv(4 - len(buf))
                        assert chunk
                        buf.extend(chunk)
                    (size,) = struct.unpack("!I", bytes(buf))
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

            send_cmd(
                trading.Command(
                    request_id="auth.1", authorize=trading.Authorize(user_id="000001")
                )
            )

            # Seed 2 long lots SHFE.rb2610 at 3500.
            send_cmd(
                trading.Command(
                    request_id="open.seed",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-seed",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "2"},
                        price={"text": "3500"},
                    ),
                )
            )
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 1 and s.positions_reconciled,
                seconds=12,
            )

            # 1) Intraday withdrawal of 200,000 causes NO false daily_loss or drawdown breach.
            write_atomic(flow / "set-funds", "1000000 800000 0 200000 7000\n")
            s = until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: s.funds.withdraw.text == "200000",
                seconds=12,
            )
            assert list(s.risk_breaches) == [], list(s.risk_breaches)
            assert s.funds.dynamic_equity.text == "800000", s.funds
            assert alerts_for("daily_loss") == []
            assert alerts_for("drawdown") == []

            # 2) Intraday deposit of 100,000 + 16,000 trading loss (baseline=900,000, equity=884,000):
            # deposit does NOT mask daily_loss; opening is blocked, webhook fires, closing is allowed.
            write_atomic(flow / "set-funds", "1000000 884000 100000 200000 7000\n")
            s = until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: "daily_loss" in s.risk_breaches,
                seconds=12,
            )
            until(lambda: alerts_for("daily_loss"), lambda items: len(items) == 1, seconds=5)
            err = send_cmd(
                trading.Command(
                    request_id="open.blocked.loss",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-blocked-loss",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                ),
                expected_error=True,
            ).error
            assert "daily_loss" in err.message, err.message

            # Closing 1 lot succeeds while daily_loss is breached.
            send_cmd(
                trading.Command(
                    request_id="close.under.loss",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-close-loss",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_SELL,
                        offset=common.OFFSET_CLOSE_TODAY,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                )
            )
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 1
                and s.positions[0].today_lots.text == "1"
                and s.positions_reconciled,
                seconds=12,
            )

            # 3) Margin ratio breach (> 70%): blocks new opens and emits webhook alert.
            write_atomic(flow / "set-funds", "1000000 900000 100000 200000 635000\n")
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: "margin_ratio" in s.risk_breaches,
                seconds=12,
            )
            until(lambda: alerts_for("margin_ratio"), lambda items: len(items) == 1, seconds=5)
            err = send_cmd(
                trading.Command(
                    request_id="open.blocked.margin",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-blocked-margin",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                ),
                expected_error=True,
            ).error
            assert "margin_ratio" in err.message, err.message

            # 4) Restore normal margin; cancel 2 resting 3-lot orders -> daily_cancels cap (2) triggers.
            write_atomic(flow / "set-funds", "1000000 900000 100000 200000 3500\n")
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: list(s.risk_breaches) == [],
                seconds=12,
            )
            for idx in (1, 2):
                oid = f"ord-rest-{idx}"
                send_cmd(
                    trading.Command(
                        request_id=f"open.rest.{idx}",
                        submit_order=trading.SubmitOrder(
                            order_id=oid,
                            instrument=dict(exchange="SHFE", symbol="rb2610"),
                            side=common.SIDE_BUY,
                            offset=common.OFFSET_OPEN,
                            lots={"text": "3"},
                            price={"text": "3500"},
                        ),
                    )
                )
                until(
                    lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                    lambda s, target=oid: any(o.id == target for o in s.working_orders),
                    seconds=12,
                )
                send_cmd(
                    trading.Command(
                        request_id=f"cancel.rest.{idx}",
                        cancel_order={"id": oid},
                    )
                )
                until(
                    lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                    lambda s, target=oid: all(o.id != target for o in s.working_orders),
                    seconds=12,
                )

            s = until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: "daily_cancels:SHFE.rb2610" in s.risk_breaches,
                seconds=12,
            )
            until(lambda: alerts_for("daily_cancels"), lambda items: len(items) == 1, seconds=5)
            err = send_cmd(
                trading.Command(
                    request_id="open.blocked.cancels",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-blocked-cancels",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                ),
                expected_error=True,
            ).error
            assert "daily_cancels" in err.message, err.message

            # 5) Settle day 1 down 26,000 from cashflow-adjusted peak (900,000 -> 874,000) and roll to 20260929:
            # daily_loss and daily_cancels clear, while multi-day drawdown (26,000 >= 25,000) persists!
            write_atomic(flow / "set-funds", "1000000 874000 100000 200000 3500\n")
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: s.funds.balance.text == "874000",
                seconds=12,
            )
            write_atomic(flow / "counter-state", "20260929 874000 874000 0 0 3500 1\n")
            write_atomic(flow / "next-day", "20260929\n")
            s = until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: s.trading_day == "2026-09-29"
                and s.broker_state == trading.BROKER_STATE_READY
                and s.positions_reconciled,
                seconds=15,
            )
            assert list(s.risk_breaches) == ["drawdown"], list(s.risk_breaches)
            assert s.funds.peak_equity.text == "900000", s.funds
            until(lambda: alerts_for("drawdown"), lambda items: len(items) == 1, seconds=5)

            # 6) Restart the Trading OS process on 2026-09-29: authorization is NOT restored;
            # after re-authorizing, drawdown still blocks opens while allowing closes.
            terminate(process)
            process = host.start("account")
            until(
                lambda: account_rpc(host, trading.Request(get_health={})).get_health,
                lambda h: h.ready,
                seconds=15,
            )
            restarted = account_rpc(host, trading.Request(get_account={})).get_account.session
            assert not restarted.HasField("authorized_ms"), restarted
            assert list(restarted.risk_breaches) == ["drawdown"], list(restarted.risk_breaches)
            assert restarted.funds.peak_equity.text == "900000", restarted.funds

            send_cmd(
                trading.Command(
                    request_id="auth.2", authorize=trading.Authorize(user_id="000001")
                )
            )
            err = send_cmd(
                trading.Command(
                    request_id="open.after.restart",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-after-restart",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                ),
                expected_error=True,
            ).error
            assert "drawdown" in err.message, err.message

            send_cmd(
                trading.Command(
                    request_id="close.after.restart",
                    submit_order=trading.SubmitOrder(
                        order_id="ord-close-restart",
                        instrument=dict(exchange="SHFE", symbol="rb2610"),
                        side=common.SIDE_SELL,
                        offset=common.OFFSET_CLOSE_YESTERDAY,
                        lots={"text": "1"},
                        price={"text": "3500"},
                    ),
                )
            )
            until(
                lambda: account_rpc(host, trading.Request(get_account={})).get_account.session,
                lambda s: len(s.positions) == 0 and s.positions_reconciled,
                seconds=12,
            )
        finally:
            terminate(process)
            server.shutdown()


if __name__ == "__main__":
    main()
