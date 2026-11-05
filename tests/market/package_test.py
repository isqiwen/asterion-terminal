"""Run the Linux release in a stock Ubuntu container with no Python installed.

The harness and test-only archive writer run on the host using prepared Lab
Python. The container runs the shipped C++ executable and vendor libraries.
Usage: <prepared-python> package_test.py <asterion-market-setup.sh>
"""
from contextlib import contextmanager
from decimal import Decimal
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

import polars as pl

sys.path.insert(0, str(Path(__file__).parents[1] / 'support'))
import market_tls
from market_wire import rpc
from asterion import market_pb2 as market


def unpack(bundle, root):
    payload = Path(bundle).read_bytes().split(b'\n__ARCHIVE__\n', 1)[1]
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
        files = {item.name for item in archive if item.isfile()}
        assert files == {'asterion-market/bin/asterion_market', 'asterion-market/bin/setup.sh',
                         'asterion-market/lib/ctp-md.so', 'asterion-market/lib/ctp-trader.so',
                         'asterion-market/config/logging.json'}, files
        archive.extractall(root, filter='data')
    root = root / 'asterion-market'
    default = Path(__file__).parents[2] / 'apps/services/market/config/logging.json'
    assert (root / 'config/logging.json').read_bytes() == default.read_bytes()
    return root


def record(root):
    folder = root / 'data/recordings/minutes/trading_day=20261028'
    folder.mkdir(parents=True)
    schema = dict(venue=pl.String, symbol=pl.String, start_ms=pl.Int64,
                  **{key: pl.Decimal(18, 8) for key in ('open', 'high', 'low', 'close')},
                  volume=pl.Int64, average_price=pl.Decimal(18, 8),
                  open_interest=pl.Decimal(18, 8), partial=pl.Boolean)
    pl.DataFrame([dict(venue='SHFE', symbol='rb2701', start_ms=1793164800000,
                      **{key: Decimal('3100.123456') for key in ('open', 'high', 'low', 'close')},
                      volume=12, average_price=None, open_interest=Decimal('2000'), partial=True)],
                 schema=schema).write_parquet(folder / 'fixture.parquet')




@contextmanager
def running(root, port):
    name = 'asterion-market-package-' + root.parent.name
    command = ['docker', 'run', '--rm', '--name', name, '--network', 'host', '--read-only',
               '--tmpfs', '/tmp', '--user', f'{os.getuid()}:{os.getgid()}',
               '-v', f'{root}:/market', '-e', 'CREDENTIALS_DIRECTORY=/market/credentials',
               'ubuntu:24.04', 'sh', '-ec',
               'if command -v python || command -v python3; then exit 1; fi; '
               '/market/bin/asterion_market --version; '
               'exec /market/bin/asterion_market']
    with (root / 'container.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(300):
                assert process.poll() is None, (root / 'container.log').read_text()
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise AssertionError((root / 'container.log').read_text())
            yield
            assert process.poll() is None, (root / 'container.log').read_text()
            processes = subprocess.check_output(['docker', 'top', name, '-eo', 'pid,comm'], text=True)
            assert [line.split()[1] for line in processes.splitlines()[1:]] == ['asterion_market'], processes
        finally:
            subprocess.run(['docker', 'stop', '-t', '30', name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
            process.wait(timeout=40)


def main():
    with tempfile.TemporaryDirectory(prefix='ast-market-package-') as folder:
        root = unpack(sys.argv[1], Path(folder))
        for name in ('credentials', 'logs'):
            (root / name).mkdir()
        record(root)
        tls = market_tls.prepare(root)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        # The container mounts the installation at /market.
        (root / 'config/market.json').write_text(json.dumps(dict(bind='127.0.0.1', port=port,
            data='/market/data', recording=dict(tick_days=30), clients=market_tls.clients(tls), acquisition=dict(exchanges=[], tushare=None),
            counter=dict(market_front='tcp://127.0.0.1:1', trade_front='tcp://127.0.0.1:1',
                         broker='fixture', user='fixture', app_id='fixture'))))
        for name in ('password', 'auth_code'):
            (root / 'credentials' / ('asterion.ctp.' + name)).write_text('fixture-only')
        with running(root, port):
            rpc(tls, port, 'start_acquisition', 'manager', id='recorded-release', provider='recorder',
                table=market.TABLE_MINUTES, contracts=['SHFE/rb/2027-01'], first_day='2026-10-28',
                last_day='2026-10-28')
            deadline = time.monotonic() + 30
            while True:
                task = rpc(tls, port, 'get_acquisition_task', id='recorded-release')
                if task.state not in (market.TASK_STATE_QUEUED, market.TASK_STATE_RUNNING):
                    break
                assert time.monotonic() < deadline, task
                time.sleep(.05)
            assert task.state == market.TASK_STATE_SUCCEEDED, task
            dataset = dict(provider='recorder', version=task.published_version)
            assert task.published_version
            def published():
                manifest = rpc(tls, port, 'get_dataset_version', dataset=dataset)
                partition, = manifest.partitions
                assert partition.rows == 1, partition
                return rpc(tls, port, 'read_dataset_file', dataset=dataset, key=partition.key).data
            before = published()
            table = pl.read_parquet(io.BytesIO(before))
            assert table['close'].to_list() == [Decimal('3100.123456')]
        # Re-extracting the release changes only its programs; persisted tasks,
        # recordings and fixed versions remain readable after a restart.
        unpack(sys.argv[1], Path(folder))
        with running(root, port):
            assert rpc(tls, port, 'get_acquisition_task', id='recorded-release') == task
            assert published() == before
    print('Pure C++ release: publication, fixed reads and restart passed without Python')


if __name__ == '__main__':
    main()
