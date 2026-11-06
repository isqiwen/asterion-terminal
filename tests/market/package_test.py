"""Run the Linux release in a stock Ubuntu container with no Python installed.

The harness and test-only archive writer run on the host using prepared Lab
Python. The container runs the shipped C++ executable and vendor libraries.
Usage: <prepared-python> package_test.py <Asterion-Market-VERSION-linux-x86_64.sh>
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
        assert files == {'asterion-market/bin/asterion_market', 'asterion-market/bin/asterion_crash_reporter', 'asterion-market/bin/setup.sh',
                         'asterion-market/lib/ctp-md.so', 'asterion-market/lib/ctp-trader.so',
                         'asterion-market/config/logging.json'}, files
        archive.extractall(root, filter='data')
    root = root / 'asterion-market'
    default = Path(__file__).parents[2] / 'apps/services/market/config/logging.json'
    assert (root / 'config/logging.json').read_bytes() == default.read_bytes()
    return root


def snapshot(root):
    (root / 'data/catalog').mkdir(parents=True)
    catalog = market.Catalog()
    item = catalog.contracts.add()
    item.contract = 'SHFE/rb/2027-01'
    item.price_tick.text = '1'
    (root / 'data/catalog/20261028.pb').write_bytes(catalog.SerializeToString())


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
            data='/market/data', clients=market_tls.clients(tls), acquisition=dict(exchanges=[], tushare=None),
            counter=dict(market_front='tcp://127.0.0.1:1', trade_front='tcp://127.0.0.1:1',
                         broker='fixture', user='fixture', app_id='fixture'))))
        for name in ('password', 'auth_code'):
            (root / 'credentials' / ('asterion.ctp.' + name)).write_text('fixture-only')
        with running(root, port):
            dataset = dict()
            deadline = time.monotonic() + 30
            while not rpc(tls, port, 'get_dataset_version', dataset=dataset).partitions:
                assert time.monotonic() < deadline
                time.sleep(.05)
            dataset['version'] = rpc(tls, port, 'get_dataset_version', dataset=dataset).dataset.version
            def published():
                manifest = rpc(tls, port, 'get_dataset_version', dataset=dataset)
                partition, = manifest.partitions
                assert partition.rows == 1, partition
                return rpc(tls, port, 'read_dataset_file', dataset=dataset, key=partition.key).data
            before = published()
            table = pl.read_parquet(io.BytesIO(before))
            assert table['price_tick'].to_list() == [Decimal('1')]
        # Re-extracting the release changes only its programs; fixed versions
        # remain readable after a restart.
        unpack(sys.argv[1], Path(folder))
        with running(root, port):
            assert published() == before
    print('Pure C++ release: snapshot intake, fixed reads and restart passed without Python')


if __name__ == '__main__':
    main()
