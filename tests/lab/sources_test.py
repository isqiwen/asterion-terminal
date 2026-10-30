"""Source isolation, simultaneous acquisition, normalization and recorder transport."""
import asyncio
from decimal import Decimal
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1)]
from asterion.lab_pb2 import Request
from asterion.recorder_pb2 import Request as RecorderRequest, Response as RecorderResponse
from asterion_lab.service import Service
from asterion_lab.sources.base import Query, SourceError
from asterion_lab.sources.akshare import Akshare
from asterion_lab.sources.recorder import Recorder
from asterion_lab.warehouse import frame


class Log:
    def write(self, *args, **kwargs):
        pass


class Records:
    def __init__(self, *rows):
        self.rows = rows

    def to_dict(self, orient):
        return self.rows


def daily(close="3100"):
    return dict(symbol="rb2701", date="20261026", open="3090", high="3110", low="3080",
                close=close, settle="3101", pre_settle="3091", volume=20, open_interest=100)


class Sources(unittest.TestCase):
    def test_normalization_and_no_unsupported_request(self):
        calls = []
        source = Akshare(fetcher=lambda *args: (calls.append(args) or Records(daily())))
        rows = source.read(Query("daily", "SHFE", "2026-10-26")).rows
        self.assertEqual(rows[0]["contract"], "SHFE/rb/2027-01")
        self.assertEqual(rows[0]["close"], Decimal("3100"))
        self.assertIsNone(rows[0]["amount"])
        frame("daily", rows)
        with self.assertRaises(SourceError) as caught:
            source.read(Query("minutes", "SHFE", "2026-10-26"))
        self.assertEqual(caught.exception.code, "unsupported")
        self.assertEqual(len(calls), 1)

    def test_duplicate_wrong_date_and_network_failure_are_not_empty_success(self):
        for records in (Records(daily(), daily()), Records(dict(daily(), date="20261025"))):
            with self.assertRaises(SourceError) as caught:
                Akshare(fetcher=lambda *args: records).read(Query("daily", "SHFE", "2026-10-26"))
            self.assertEqual(caught.exception.code, "invalid_data")
        def failed(*args):
            raise OSError("private diagnostic")
        with self.assertRaises(SourceError) as caught:
            Akshare(fetcher=failed).read(Query("daily", "SHFE", "2026-10-26"))
        self.assertEqual(caught.exception.code, "unavailable")
        self.assertNotIn("private", str(caught.exception))

    def test_two_sources_run_together_and_keep_separate_versions(self):
        with tempfile.TemporaryDirectory() as folder:
            service = Service("lab", Path(folder), Log())
            barrier = threading.Barrier(2)
            def factory(provider, cancelled=lambda: False):
                def fetch(*args):
                    barrier.wait(5)  # Both must run; serializing all sources fails.
                    return Records(daily("3100" if provider == "tushare" else "3102"))
                return Akshare(cancelled=cancelled, fetcher=fetch)
            async def request(operation, **values):
                req = Request(version=1, service_id="lab")
                getattr(req, operation).SetInParent()
                for key, value in values.items():
                    field = getattr(getattr(req, operation), key)
                    if isinstance(value, list):
                        field.extend(value)
                    else:
                        setattr(getattr(req, operation), key, value)
                return await service.reply(req)
            try:
                with patch.object(service, "source", side_effect=factory):
                    for provider in ("tushare", "akshare"):
                        reply = asyncio.run(request("acquire", id=provider, provider=provider,
                            table="daily", exchanges=["SHFE"], begin_day="2026-10-26", end_day="2026-10-26"))
                        self.assertEqual(reply.WhichOneof("body"), "accepted", str(reply))
                    duplicate = asyncio.run(request("acquire", id="tushare", provider="akshare"))
                    self.assertEqual(duplicate.error.code, "invalid_request")
                    for thread in threading.enumerate():
                        if thread.name in ("acquisition-tushare", "acquisition-akshare"):
                            thread.join(10)
                versions = []
                for provider, close in (("tushare", "3100"), ("akshare", "3102")):
                    task = service.acquisitions[provider].tasks[provider]
                    self.assertEqual(task.state, "succeeded", task.error)
                    warehouse = service.warehouse_for(provider)
                    version = warehouse.current()
                    self.assertEqual(version.manifest["provider"], provider)
                    versions.append(version.identity)
                    rows = warehouse.query(version, "select close from daily", ("daily",))
                    self.assertEqual(rows[0][0], Decimal(close))
                    reply = asyncio.run(request("summary", provider=provider))
                    self.assertEqual(reply.summary.dataset_version, version.identity)
                self.assertNotEqual(*versions)
                with self.assertRaises(SourceError) as unsupported:
                    service.daily_bars("SHFE/rb/2027-01", "2026-10-26", "2026-10-26", "recorder")
                self.assertEqual(unsupported.exception.code, "unsupported")
                reply = asyncio.run(request("summary", provider="missing"))
                self.assertEqual(reply.error.code, "invalid_request")
            finally:
                service.reads.shutdown()

    def test_cancel_during_fetch_does_not_publish_the_partition(self):
        from asterion_lab.acquisition import Acquisition
        from asterion_lab.warehouse import Warehouse
        with tempfile.TemporaryDirectory() as folder:
            owner = Acquisition(Warehouse(Path(folder), "akshare"), Log())
            entered, release = threading.Event(), threading.Event()
            def fetch(*args):
                entered.set()
                release.wait(5)
                return Records(daily())
            task = owner.submit("cancel", "2026-10-26", "2026-10-26",
                lambda cancelled: Akshare(cancelled, fetch), exchanges=["SHFE"], provider="akshare")
            self.assertTrue(entered.wait(5))
            owner.cancel("cancel")
            release.set()
            for thread in threading.enumerate():
                if thread.name == "acquisition-cancel":
                    thread.join(5)
            self.assertEqual(task.state, "cancelled")
            self.assertEqual(owner.warehouse.current().identity, "")

    def test_closed_days_are_skipped_or_left_uncovered(self):
        from urllib.error import HTTPError
        from asterion_lab.acquisition import Acquisition
        from asterion_lab.sources.exchanges import Shfe
        from asterion_lab.warehouse import Warehouse
        asked = []
        def settlement(exchange, day):
            asked.append(day)
            if day == "2026-10-01":  # National Day: SHFE publishes no file.
                raise HTTPError("https://www.shfe.com.cn", 404, "Not Found", None, None)
            if day == "2026-10-05":
                raise HTTPError("https://www.shfe.com.cn", 503, "Unavailable", None, None)
            return {"SHFE/rb/2027-01": {"long_margin_rate": Decimal("0.1")}}
        with tempfile.TemporaryDirectory() as folder, \
                patch("asterion_lab.sources.exchanges.official_settlement", settlement):
            owner = Acquisition(Warehouse(Path(folder), "shfe"), Log())
            def run(identity, begin, end):
                task = owner.submit(identity, begin, end, lambda cancelled: Shfe(cancelled),
                                    exchanges=["SHFE"], provider="shfe", table="settlement")
                for thread in threading.enumerate():
                    if thread.name == f"acquisition-{identity}":
                        thread.join(5)
                return task
            # Thursday to Friday across the weekend: no request for Saturday or Sunday.
            task = run("holiday", "2026-10-01", "2026-10-04")
            self.assertEqual(task.state, "succeeded", task.error)
            self.assertEqual(asked, ["2026-10-01", "2026-10-02"])
            self.assertEqual((task.days_total, task.days_done, task.days_empty), (2, 2, 1))
            version = owner.warehouse.current()
            self.assertTrue(version.has("settlement/SHFE/2026-10-02"))
            self.assertFalse(version.has("settlement/SHFE/2026-10-01"))
            # Any other refusal still fails the task.
            failed = run("unavailable", "2026-10-05", "2026-10-05")
            self.assertEqual((failed.state, failed.error_code), ("failed", "unavailable"))

    def test_recorder_preserves_partial_minutes_and_observation_gaps(self):
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen()
        failures = []
        def serve():
            try:
                for _ in range(2):
                    with server.accept()[0] as connection:
                        stream = connection.makefile("rb")
                        self.assertEqual(stream.read(4), b"ASTR")
                        size, = struct.unpack("!I", stream.read(4))
                        request = RecorderRequest.FromString(stream.read(size))
                        response = RecorderResponse(version=1, correlation_id=request.correlation_id)
                        if request.WhichOneof("body") == "days":
                            day = response.days.days.add(trading_day="20261026")
                            for number, start in ((1, 1000), (2, 9000)):
                                day.segments.add(instance="a", number=number, first_received_ms=start,
                                    last_received_ms=start+1000, ticks=2, end="disconnected")
                        else:
                            response.minutes.trading_day = request.minutes.trading_day
                            response.minutes.venue = request.minutes.venue
                            response.minutes.symbol = request.minutes.symbol
                            response.minutes.bars.add(start_ms=1000, close="3100.123456", volume=2, partial=True)
                        data = response.SerializeToString()
                        connection.sendall(struct.pack("!I", len(data))+data)
            except Exception as error:
                failures.append(error)
            finally:
                server.close()
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        source = Recorder(*server.getsockname())
        result = source.read(Query("minutes", "SHFE", "2026-10-26", "SHFE/rb/2027-01"))
        thread.join(5)
        self.assertFalse(failures)
        self.assertTrue(result.rows[0]["partial"])
        self.assertEqual(result.rows[0]["close"], Decimal("3100.123456"))
        self.assertEqual(len(result.observations), 2)
        self.assertEqual(result.observations[1]["first_received_ms"], 9000)
        frame("minutes", result.rows)
        frame("observations", result.observations)


if __name__ == "__main__":
    unittest.main()
