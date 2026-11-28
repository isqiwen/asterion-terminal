"""Automated post-close inspection (audit + reconcile + webhook) tests.

Run with the bundled interpreter:
python3 -I -B post_close_test.py <source> <generated-python> <engine-directory>
"""
from decimal import Decimal
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]

from asterion.common_pb2 import Decimal as WireDecimal, Instrument, Period  # noqa: E402
from asterion.strategy_pb2 import (  # noqa: E402
    Bar as WireBar,
    Bars as WireBars,
    Decision,
    DominantSeries,
    Instance,
    SeriesContract,
)
from asterion_lab import backtest, package, post_close, reconcile_diff, repository  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402
from warehouse_writer import Warehouse  # noqa: E402

EXAMPLES = Path(__file__).resolve().parents[2] / "research"


def _wdec(value) -> WireDecimal:
    text = format(Decimal(str(value)), "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return WireDecimal(text="0" if text in ("", "-0") else text)


def _native_symbol(canonical: str) -> Instrument:
    ex, prod, month = canonical.split("/")
    suffix = month[3:4] + month[5:] if ex == "CZCE" else month[2:4] + month[5:]
    return Instrument(exchange=ex, symbol=f"{prod}{suffix}")


def _write_trading_account(account_dir: Path, instance: Instance, steps: list, orders: list, fills: list):
    strat_dir = account_dir / "strategies"
    (strat_dir / "runs").mkdir(parents=True, exist_ok=True)
    run_db = strat_dir / "runs" / f"{instance.id}.sqlite"
    run_db.unlink(missing_ok=True)
    with sqlite3.connect(run_db) as conn:
        conn.execute("CREATE TABLE configuration (value BLOB NOT NULL)")
        conn.execute("CREATE TABLE steps (sequence INTEGER PRIMARY KEY, input BLOB NOT NULL, decision BLOB)")
        conn.execute("CREATE TABLE events (time_ms INTEGER NOT NULL, kind INTEGER NOT NULL, event TEXT NOT NULL)")
        conn.execute("INSERT INTO configuration VALUES (?)", (instance.SerializeToString(),))
        for seq, wire_bars, decision in steps:
            conn.execute(
                "INSERT INTO steps VALUES (?, ?, ?)",
                (seq, wire_bars.SerializeToString(), None if decision is None else decision.SerializeToString()),
            )
    journal_db = account_dir / "journal.sqlite"
    journal_db.unlink(missing_ok=True)
    with sqlite3.connect(journal_db) as conn:
        conn.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
        conn.execute("CREATE TABLE records(sequence INTEGER PRIMARY KEY, body TEXT NOT NULL)")
        conn.execute(
            "CREATE TABLE segments(first INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
            "digest TEXT NOT NULL, records INTEGER NOT NULL, bytes INTEGER NOT NULL)"
        )
        conn.execute("INSERT INTO meta VALUES ('format', 'asterion.journal')")
        seq = 1
        for o in orders:
            conn.execute("INSERT INTO records VALUES (?, ?)", (seq, json.dumps(o)))
            seq += 1
        if fills:
            conn.execute("INSERT INTO records VALUES (?, ?)", (seq, json.dumps({"trades_recorded": fills})))


class _WebhookRecorder:
    def __init__(self, statuses: list[int] | None = None):
        self.received: list[tuple[str, dict]] = []
        self._statuses = list(statuses or [])
        recorder = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length)
                recorder.received.append((self.path, json.loads(raw.decode("utf-8"))))
                code = recorder._statuses.pop(0) if recorder._statuses else 200
                self.send_response(code)
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, format, *args):  # noqa: A002
                pass

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


class PostCloseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lab_dir = self.root / "lab"
        self.lab_dir.mkdir()
        warehouse(self.lab_dir)
        self.version = Warehouse(self.lab_dir).current()
        self.repo = self.root / "repo"
        shutil.copytree(EXAMPLES, self.repo)
        self.account_dir = self.root / "acct-alpha"
        self.account_dir.mkdir()
        self._prepare_matching_account()

    def tearDown(self):
        self.tmp.cleanup()

    def _prepare_matching_account(self):
        cfg = backtest.Config(
            repository=str(self.repo),
            strategy="ma_cross.MaCross",
            params={"fast": 3, "slow": 6, "lots": 2},
            symbols=("SHFE/rb",),
            begin_day=DAYS[0].isoformat(),
            end_day=DAYS[-1].isoformat(),
            deposit=Decimal("1000000"),
            costs=None,
            slippage_ticks=0,
            max_order_lots=Decimal("10"),
            max_gross_lots=Decimal("20"),
            max_contract_lots=Decimal("20"),
            max_working_orders=10,
            dataset_version=self.version.identity,
            source=str(self.repo),
        )
        backtest.run(self.lab_dir, "bt-1", cfg)
        strat = repository.load(self.repo, cfg.strategy, cfg.params)
        replayed = reconcile_diff.replay_backtest(Warehouse(self.lab_dir), cfg, strat)

        pkg = package.build(self.repo, cfg.strategy)
        pkg_dir = self.account_dir / "strategies" / "packages" / pkg.manifest.name / pkg.manifest.version
        pkg_dir.mkdir(parents=True, exist_ok=True)
        for f in pkg.files:
            target = pkg_dir / f.path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(f.content)
        (pkg_dir / "strategy.json").write_text(json.dumps({
            "name": pkg.manifest.name,
            "version": pkg.manifest.version,
            "entry": pkg.manifest.entry,
            "runtime": pkg.manifest.runtime,
            "files": list(pkg.manifest.files),
        }))

        instance = Instance(
            id="inst-1",
            package_name=pkg.manifest.name,
            package_version=pkg.manifest.version,
            period=Period.PERIOD_DAY,
            series=[
                DominantSeries(
                    exchange="SHFE",
                    product="rb",
                    contracts=[
                        SeriesContract(instrument=_native_symbol("SHFE/rb/2026-05"), delivery_month="2026-05"),
                        SeriesContract(instrument=_native_symbol("SHFE/rb/2026-10"), delivery_month="2026-10"),
                    ],
                )
            ],
        )
        instance.params["fast"].integer = 3
        instance.params["slow"].integer = 6
        instance.params["lots"].integer = 2

        steps = []
        for seq, (end_ms, dec) in enumerate(sorted(replayed["decisions"].items()), 1):
            b = replayed["bars"][(end_ms, "SHFE/rb")]
            wire_bar = WireBar(
                instrument=_native_symbol(b["contract"]),
                trading_day=b["trading_day"],
                end_ms=end_ms,
                open=_wdec(b["open"]),
                high=_wdec(b["high"]),
                low=_wdec(b["low"]),
                close=_wdec(b["close"]),
                volume=int(b["volume"]),
                open_interest=_wdec(b["open_interest"]),
            )
            if b["roll_ratio"] is not None:
                wire_bar.roll_ratio.CopyFrom(_wdec(b["roll_ratio"]))
            if b["roll_yield"] is not None:
                wire_bar.roll_yield.CopyFrom(_wdec(b["roll_yield"]))
            decision = Decision(sequence=seq, ready=dec["ready"])
            for target in dec["targets"].values():
                decision.targets.add(instrument=_native_symbol(b["contract"]), lots=int(target["value"]))
            steps.append((seq, WireBars(sequence=seq, bars=[wire_bar]), decision))

        orders, fills = [], []
        for idx, row in enumerate(replayed["fills"].iter_rows(named=True), 1):
            inst = _native_symbol(row["contract"])
            oid = f"ord-{idx}"
            day = row["trading_day"].isoformat()
            orders.append({
                "command": {
                    "action": "submit",
                    "order_id": oid,
                    "venue": inst.exchange,
                    "symbol": inst.symbol,
                    "side": row["side"],
                    "offset": row["offset"],
                    "quantity": str(Decimal(row["quantity"]).normalize()),
                    "price": str(Decimal(row["price"]).normalize()),
                },
                "policy_revision": "p1",
                "broker_key": f"bk-{idx}",
                "trading_day": day,
                "strategy": {"instance": "inst-1", "target_sequence": idx, "leg": 0},
            })
            fills.append({
                "id": f"trd-{idx}",
                "order_id": oid,
                "venue": inst.exchange,
                "symbol": inst.symbol,
                "side": row["side"],
                "offset": row["offset"],
                "quantity": str(Decimal(row["quantity"]).normalize()),
                "price": str(Decimal(row["price"]).normalize()),
                "trading_day": day,
                "trade_time": "09:00:01",
                "received_ms": idx * 1000,
            })
        self._instance = instance
        self._steps = steps
        self._orders = orders
        self._fills = fills
        _write_trading_account(self.account_dir, instance, steps, orders, fills)

    def test_clean_inspection_and_always_notify(self):
        recorder = _WebhookRecorder()
        self.addCleanup(recorder.close)
        secret_url = f"http://127.0.0.1:{recorder.port}/hook/secret-token-xyz"
        out_path = self.root / "clean.json"

        with self.assertRaises(SystemExit) as ctx:
            post_close.main([
                "--lab-dir", str(self.lab_dir),
                "--version", self.version.identity,
                "--oi-jump-ratio", "12",
                "--trading-account", f"{self.account_dir}::inst-1::bt-1",
                "--strategy-source", str(self.repo),
                "--webhook-url", secret_url,
                "--output", str(out_path),
            ])
        self.assertEqual(ctx.exception.code, 0)
        raw_out = out_path.read_text(encoding="utf-8")
        self.assertNotIn("secret-token-xyz", raw_out)
        report = json.loads(raw_out)
        self.assertTrue(report["ok"], report)
        self.assertTrue(report["audit"]["ok"])
        self.assertTrue(report["accounts"][0]["ok"])
        self.assertEqual(report["webhook"]["status"], "skipped_ok")
        self.assertEqual(len(recorder.received), 0)

        # --always-notify with --warehouse-dir (resolving current) pushes post_close_ok.
        with self.assertRaises(SystemExit) as ctx:
            post_close.main([
                "--warehouse-dir", str(self.lab_dir),
                "--lab-dir", str(self.lab_dir),
                "--oi-jump-ratio", "12",
                "--trading-account", f"{self.account_dir}::inst-1::bt-1",
                "--strategy-source", str(self.repo),
                "--webhook-url", secret_url,
                "--always-notify",
            ])
        self.assertEqual(ctx.exception.code, 0)
        self.assertEqual(len(recorder.received), 1)
        path, payload = recorder.received[0]
        self.assertEqual(path, "/hook/secret-token-xyz")
        self.assertEqual(payload["service"], "asterion-post-close")
        self.assertEqual(payload["kind"], "post_close_ok")
        self.assertEqual(payload["account"], "acct-alpha")
        self.assertIn("details", payload)

    def test_anomaly_alert_deduplication_recovery_and_webhook_file(self):
        recorder = _WebhookRecorder()
        self.addCleanup(recorder.close)

        # Inject a daily OHLC error in Warehouse and a fill slippage mismatch in acct-alpha.
        store = Warehouse(self.lab_dir)
        jan_rows = [
            dict(r, day=r["day"].isoformat())
            for r in store.read(self.version, "daily/SHFE/2026-01").to_dicts()
        ]
        for r in jan_rows:
            if r["contract"] == "SHFE/rb/2026-05" and r["day"] == DAYS[10].isoformat():
                r["high"] = Decimal("2000")  # invalid: high < close
        writer = store.writer()
        writer.put_rows("daily/SHFE/2026-01", "daily", jan_rows, days=sorted({r["day"] for r in jan_rows}))
        bad_version = writer.commit()

        slipped_fills = [dict(f) for f in self._fills]
        slipped_fills[0]["price"] = str(Decimal(slipped_fills[0]["price"]) + Decimal("3"))
        _write_trading_account(self.account_dir, self._instance, self._steps, self._orders, slipped_fills)

        idle_account = self.root / "acct-idle"
        idle_account.mkdir()

        hook_file = self.root / "webhook.secret"
        hook_file.write_text(f"http://127.0.0.1:{recorder.port}/alerts/top-secret\n", encoding="utf-8")
        state_file = self.root / "post-close.state.json"
        out_path = self.root / "anomaly.json"

        argv = [
            "--warehouse-dir", str(self.lab_dir),
            "--lab-dir", str(self.lab_dir),
            "--version", bad_version.identity,
            "--oi-jump-ratio", "12",
            "--trading-account", f"{self.account_dir}::inst-1::bt-1",
            "--trading-account", str(idle_account),
            "--allow-no-instance",
            "--strategy-source", str(self.repo),
            "--webhook-file", str(hook_file),
            "--state-file", str(state_file),
            "--output", str(out_path),
        ]

        # Run 1: detects audit + reconcile failures, skips idle account, sends alert.
        with self.assertRaises(SystemExit) as ctx:
            post_close.main(argv)
        self.assertEqual(ctx.exception.code, 1)
        raw_out = out_path.read_text(encoding="utf-8")
        self.assertNotIn("top-secret", raw_out)
        self.assertNotIn("top-secret", state_file.read_text(encoding="utf-8"))
        report = json.loads(raw_out)
        self.assertFalse(report["ok"])
        self.assertFalse(report["audit"]["ok"])
        self.assertFalse(report["accounts"][0]["ok"])
        self.assertTrue(report["accounts"][1]["skipped"])
        self.assertEqual(report["webhook"]["status"], "sent")
        self.assertEqual(len(recorder.received), 1)
        _, payload = recorder.received[0]
        self.assertEqual(payload["kind"], "post_close_alert")
        self.assertIn("数据质量异常", payload["text"])
        self.assertIn("acct-alpha", payload["text"])

        # Run 2: identical failure is deduplicated by state_file.
        with self.assertRaises(SystemExit) as ctx:
            post_close.main(argv)
        self.assertEqual(ctx.exception.code, 1)
        self.assertEqual(json.loads(out_path.read_text(encoding="utf-8"))["webhook"]["status"], "deduplicated")
        self.assertEqual(len(recorder.received), 1)

        # Run 3: clean run resets deduplication state.
        with self.assertRaises(SystemExit) as ctx:
            post_close.main([
                "--skip-audit",
                "--lab-dir", str(self.lab_dir),
                "--trading-account", str(idle_account),
                "--allow-no-instance",
                "--webhook-file", str(hook_file),
                "--state-file", str(state_file),
            ])
        self.assertEqual(ctx.exception.code, 0)
        self.assertEqual(len(recorder.received), 1)

        # Run 4: recurrence after recovery triggers a new alert.
        with self.assertRaises(SystemExit) as ctx:
            post_close.main(argv)
        self.assertEqual(ctx.exception.code, 1)
        self.assertEqual(len(recorder.received), 2)

    def test_webhook_validation_and_retry_without_leaking_url(self):
        for bad in ("", "ftp://example.com/hook", "https://", "http://host/has space", "https://:8080/a"):
            with self.subTest(url=bad), self.assertRaises(ValueError) as caught:
                post_close.validate_webhook_url(bad)
            if bad:
                self.assertNotIn(bad, str(caught.exception))

        # 503 is retried and succeeds on attempt 2.
        flaky = _WebhookRecorder(statuses=[503, 200])
        self.addCleanup(flaky.close)
        status = post_close.send_webhook(
            f"http://127.0.0.1:{flaky.port}/secret-retry",
            {"kind": "test"},
            retry_delays=(0.01, 0.01),
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(flaky.received), 2)

        # 400 is not retried and never embeds the URL in the exception.
        reject = _WebhookRecorder(statuses=[400, 200])
        self.addCleanup(reject.close)
        with self.assertRaises(RuntimeError) as caught:
            post_close.send_webhook(
                f"http://127.0.0.1:{reject.port}/secret-reject",
                {"kind": "test"},
                retry_delays=(0.01,),
            )
        self.assertEqual(len(reject.received), 1)
        self.assertIn("HTTP 400", str(caught.exception))
        self.assertNotIn("secret-reject", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
