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
            with history_tls.connect(tls, port) as slow:
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                # Pipelined one-MiB replies exceed the sender's socket buffer.
                # This client reads nothing while the healthy clients keep working.
                for _ in range(16):
                    send(slow, 'read_dataset_file', **file_query)
                for _ in range(12):
                    assert call('read_dataset_file', **file_query) == baseline
            publication = call('get_dataset_version', dataset=dict(version=fixed))
            assert call('read_dataset_file', **file_query) == baseline
        # A task still running when the process died comes back interrupted.
        unfinished = market.AcquisitionTask(state=market.TASK_STATE_RUNNING, started_ms=1)
        unfinished.request.id = 'interrupted'
        (root / 'data/acquisition/interrupted.pb').write_bytes(unfinished.SerializeToString())
        with running(root, port):
            recovered = call('get_acquisition_task', id='interrupted')
            assert recovered.state == market.TASK_STATE_INTERRUPTED, recovered
            assert call('get_status').started_ms != initial.started_ms
            assert call('get_dataset_version', dataset=dict(version=fixed)) == publication
            assert call('read_dataset_file', **file_query) == baseline
            stable = recovered.SerializeToString()
            time.sleep(.2)
            assert call('get_acquisition_task', id='interrupted').SerializeToString() == stable
        print('Slow clients, fixed versions and crash recovery passed')


if __name__ == '__main__':
    main()
