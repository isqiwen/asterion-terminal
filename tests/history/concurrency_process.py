"""Isolated C++ History: stalled readers, live recording, publication and crash recovery.

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
import history_tls
from asterion import history_pb2 as market
from history_installation import history_host, running
from history_wire import rpc, send, receive
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
    writer = Warehouse(root / 'data', 'research').writer()
    writer.put_rows('daily/SHFE/2026-10', 'daily', rows,
                    days=[f'2026-10-{day:02}' for day in range(1, 29)])
    version = writer.commit()
    part = version.manifest['partitions']['daily/SHFE/2026-10']
    assert (root / 'data/sources/research/warehouse' / part['file']).stat().st_size > 1024 * 1024
    return version.identity


def main():
    with tempfile.TemporaryDirectory(prefix='ast-market-concurrency-') as folder:
        root = Path(folder) / 'market'
        with history_host(build, root) as port:
            tls = root / 'config/tls'
            fixed = history(root)
            def call(operation, **values):
                return rpc(tls, port, operation, **values)
            initial = call('get_status')
            file_query = dict(dataset=dict(version=fixed), key='daily/SHFE/2026-10')
            baseline = call('read_dataset_file', **file_query)
            # The day's contract snapshot is taken in while a slow client stalls and others read.
            from asterion import quotes_pb2
            catalog = quotes_pb2.Catalog()
            item = catalog.contracts.add()
            item.contract = 'SHFE/rb/2027-01'
            item.price_tick.text = '1'
            (root / 'data/catalog').mkdir(parents=True, exist_ok=True)
            (root / 'data/catalog/20261028.pb').write_bytes(catalog.SerializeToString())
            with history_tls.connect(tls, port) as slow:
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                # Pipelined one-MiB replies exceed the sender's socket buffer.
                # This client reads nothing while the healthy clients keep working.
                for _ in range(16):
                    send(slow, 'read_dataset_file', **file_query)
                for _ in range(12):
                    assert call('read_dataset_file', **file_query) == baseline
                publication = until(lambda: call('get_dataset_version', table=market.TABLE_PRICE_TICKS, dataset={}),
                                    lambda v: v.partitions, timeout=30)
            recorder = dict(version=publication.dataset.version)
            assert call('read_dataset_file', **file_query) == baseline
        # Reopen it, then kill a running task.
        with running(root, port) as process:
            assert call('get_status').started_ms != initial.started_ms
            call('start_acquisition', role='manager', id='interrupted',
                 table=market.TABLE_PRICE_TICKS, first_day='1990-01-01', last_day='2026-10-27')
            progress = until(lambda: call('get_acquisition_task', id='interrupted'),
                             lambda t: t.state == market.TASK_STATE_RUNNING and t.items_done > 0)
            # The submitting client has gone; collection and task work continue.
            assert progress.items_done < progress.items
            process.kill()
            process.wait(timeout=10)
        with running(root, port):
            recovered = call('get_acquisition_task', id='interrupted')
            assert recovered.state == market.TASK_STATE_INTERRUPTED, recovered
            assert call('get_dataset_version', table=market.TABLE_PRICE_TICKS, dataset=recorder) == publication
            assert call('read_dataset_file', **file_query) == baseline
            stable = recovered.SerializeToString()
            time.sleep(.2)
            assert call('get_acquisition_task', id='interrupted').SerializeToString() == stable
        print('Live intake, slow/history clients, autonomous publication and crash recovery passed')


if __name__ == '__main__':
    main()
