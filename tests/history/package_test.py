"""Run the Linux release in a stock Ubuntu container with no Python installed.

The harness and test-only archive writer run on the host using prepared Lab
Python. The container runs the shipped C++ executable and vendor libraries.
Usage: <prepared-python> package_test.py <Asterion-History-VERSION-linux-x86_64.sh>
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
import history_tls
from history_wire import rpc
from asterion import history_pb2 as market
sys.path[:0] = [str(Path(__file__).parents[1] / 'lab'), str(Path(__file__).parents[2] / 'apps/services/lab')]
from warehouse_writer import Warehouse


def unpack(bundle, root):
    payload = Path(bundle).read_bytes().split(b'\n__ARCHIVE__\n', 1)[1]
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
        files = {item.name for item in archive if item.isfile()}
        assert files == {'asterion-history/bin/asterion_history', 'asterion-history/bin/asterion_crash_reporter', 'asterion-history/bin/setup.sh',
                         'asterion-history/config/logging.json'}, files
        archive.extractall(root, filter='data')
    root = root / 'asterion-history'
    default = Path(__file__).parents[2] / 'apps/services/history/config/logging.json'
    assert (root / 'config/logging.json').read_bytes() == default.read_bytes()
    return root


def publish(root):
    writer = Warehouse(root / 'data').writer()
    writer.put_rows('price_ticks/SHFE', 'price_ticks', [dict(
        exchange='SHFE', product='rb', contract='SHFE/rb/2027-01', effective_from='2026-10-28',
        effective_until='2026-10-29', price_tick='1', source_url='fixture')])
    return writer.commit().identity


@contextmanager
def running(root, port):
    name = 'asterion-history-package-' + root.parent.name
    command = ['docker', 'run', '--rm', '--platform', 'linux/amd64', '--name', name, '--network', 'host', '--read-only',
               '--tmpfs', '/tmp', '--user', f'{os.getuid()}:{os.getgid()}',
               '-v', f'{root}:/market', '-e', 'CREDENTIALS_DIRECTORY=/market/credentials',
               'ubuntu:24.04', 'sh', '-ec',
               'if command -v python || command -v python3; then exit 1; fi; '
               '/market/bin/asterion_history --version; '
               'exec /market/bin/asterion_history']
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
            # The kernel keeps 15 characters of a process name.
            assert [line.split()[1] for line in processes.splitlines()[1:]] == ['asterion_histor'], processes
        finally:
            subprocess.run(['docker', 'stop', '-t', '30', name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
            process.wait(timeout=40)


def main():
    with tempfile.TemporaryDirectory(prefix='ast-market-package-') as folder:
        root = unpack(sys.argv[1], Path(folder))
        for name in ('credentials', 'logs'):
            (root / name).mkdir()
        fixed = publish(root)
        tls = history_tls.prepare(root)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        # The container mounts the installation at /market.
        (root / 'config/history.json').write_text(json.dumps(dict(bind='127.0.0.1', port=port,
            data='/market/data', clients=history_tls.clients(tls), acquisition=dict(exchanges=[], tushare=None))))
        with running(root, port):
            dataset = dict(version=fixed)
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
    print('Pure C++ release: fixed reads and restart passed without Python')


if __name__ == '__main__':
    main()
