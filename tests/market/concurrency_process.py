"""Isolated C++ Market: stalled readers, live recording, publication and crash recovery.

All data and credentials are explicit test fixtures. No provider network is used.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
from decimal import Decimal
from pathlib import Path
import random
import socket
import sys
import tempfile
import time

source, build = Path(sys.argv[1]), Path(sys.argv[2])
sys.path[:0] = [str(source / 'tests/support'), str(source / 'tests/lab'),
               str(source / 'apps/services/lab'), str(build / 'generated-python')]
import market_tls
from asterion import acquisition_pb2 as acquisition
from market_installation import market_host, running_market
from market_wire import rpc, send, receive
from warehouse_writer import Warehouse
from warehouse_fixture import daily


def until(operation, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        value = operation()
        if predicate(value):
            return value
        assert time.monotonic() < deadline, value
        time.sleep(.03)


def history(root):
    # A test-only monthly partition large enough to fill a nonreading socket.
    rng = random.Random(306)
    rows = [daily(f'SHFE/x{i // 28}/2027-01', date(2026, 10, i % 28 + 1),
                  Decimal(rng.randrange(1000000000, 1000000000000)).scaleb(-6), 2000)
            for i in range(60000)]
    writer = Warehouse(root / 'data', 'tushare').writer()
    writer.put_rows('daily/SHFE/2026-10', 'daily', rows,
                    days=[f'2026-10-{day:02}' for day in range(1, 29)])
    version = writer.commit()
    part = version.manifest['partitions']['daily/SHFE/2026-10']
    assert (root / 'data/warehouse' / part['file']).stat().st_size > 1024 * 1024
    return version.identity


def main():
    with tempfile.TemporaryDirectory(prefix='ast-market-concurrency-') as folder:
        root = Path(folder) / 'market'
        with market_host(build, root, user='concurrency') as port:
            tls = root / 'config/tls'
            fixed = history(root)
            def call(operation, **values):
                return rpc(tls, port, operation, **values)
            initial = until(lambda: call('get_live_snapshot').snapshot,
                            lambda s: s.phase == 'connected' and bool(s.quotes))
            instance = initial.instance_id
            selected = initial.quotes[0].instrument
            file_query = dict(provider='tushare', dataset_version=fixed, key='daily/SHFE/2026-10')
            baseline = call('read_dataset_file', **file_query).dataset_file
            recorded = until(lambda: call('get_recording_status').recording_status,
                             lambda s: s.rows > 0 and s.files > 0, timeout=40)
            assert not recorded.write_error and recorded.lost == 0, recorded
            day = recorded.trading_day
            trading_day = f'{day[:4]}-{day[4:6]}-{day[6:]}'
            with market_tls.connect(tls, port) as slow, market_tls.connect(tls, port) as watch:
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                # Pipelined one-MiB replies exceed the sender's socket buffer.
                # This client reads nothing while the healthy clients keep working.
                for _ in range(16):
                    send(slow, 'read_dataset_file', **file_query)
                send(watch, 'stream_live_quotes', instruments=[selected])
                first = receive(watch).live_quotes.snapshot
                sequence = first.sequence
                # The native publication runs while readers and the live watch proceed;
                # its submitting connection closes immediately after acceptance.
                call('start_acquisition', role='manager', id='publish', provider='recorder',
                     table='minutes', contract='SHFE/rb/2026-10',
                     begin_day=(date.fromisoformat(trading_day) - timedelta(days=30)).isoformat(),
                     end_day=trading_day)
                with ThreadPoolExecutor(max_workers=1) as readers:
                    def read_history():
                        for _ in range(12):
                            reply = call('read_dataset_file', **file_query).dataset_file
                            assert reply == baseline
                    reading = readers.submit(read_history)
                    advancing = 0
                    deadline = time.monotonic() + 4
                    while time.monotonic() < deadline:
                        frame = receive(watch).live_quotes.delta
                        assert frame.instance_id == instance
                        assert frame.base_sequence == sequence
                        advancing += frame.sequence > sequence
                        sequence = frame.sequence
                    reading.result()
                    assert advancing >= 5, 'live watch stopped behind a slow/history reader'
            before = call('get_live_snapshot').snapshot.sequence
            # No clients remain connected; provider intake and the writer keep running.
            time.sleep(.4)
            assert call('get_live_snapshot').snapshot.sequence > before
            task = until(lambda: call('get_acquisition_task', id='publish').acquisition_task,
                         lambda t: t.state not in (acquisition.TASK_STATE_QUEUED, acquisition.TASK_STATE_RUNNING))
            assert task.state == acquisition.TASK_STATE_SUCCEEDED and task.dataset_version, task
            publication = call('get_dataset_version', provider='recorder', dataset_version=task.dataset_version).dataset
            assert publication.partitions
            assert call('read_dataset_file', **file_query).dataset_file == baseline
        # Stop above flushes the live archive. Reopen it, then kill a running task.
        with running_market(root, port) as process:
            restarted = call('get_live_snapshot').snapshot.instance_id
            assert restarted != instance
            call('start_acquisition', role='manager', id='interrupted', provider='recorder',
                 table='minutes', contract='SHFE/rb/2026-10',
                 begin_day='1990-01-01', end_day=trading_day)
            progress = until(lambda: call('get_acquisition_task', id='interrupted').acquisition_task,
                             lambda t: t.state == acquisition.TASK_STATE_RUNNING and t.days_done > 0)
            # The submitting client has gone; collection and task work continue.
            assert progress.days_done < progress.days_total
            process.kill()
            process.wait(timeout=10)
        with running_market(root, port):
            recovered = call('get_acquisition_task', id='interrupted').acquisition_task
            assert recovered.state == acquisition.TASK_STATE_INTERRUPTED, recovered
            assert call('get_acquisition_task', id='publish').acquisition_task == task
            assert call('get_dataset_version', provider='recorder', dataset_version=task.dataset_version).dataset == publication
            assert call('read_dataset_file', **file_query).dataset_file == baseline
            stable = recovered.SerializeToString()
            time.sleep(.2)
            assert call('get_acquisition_task', id='interrupted').acquisition_task.SerializeToString() == stable
            until(lambda: call('get_recording_status').recording_status, lambda s: s.last_quote_ms > 0)
        print('Live intake, slow/history clients, autonomous publication and crash recovery passed')


if __name__ == '__main__':
    main()
