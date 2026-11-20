"""Run the Linux Market release offline in a stock Ubuntu container without Python.

The harness runs on the host with stdlib Python only; the container runs the
shipped C++ executable, CTP SDK libraries and installer upgrade path against a
closed loopback front so no external provider is contacted.
Usage: python3 tests/market/package_test.py <Asterion-Market-VERSION-linux-x86_64.sh>
"""
from contextlib import contextmanager
import io
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, str(Path(__file__).parents[1] / 'support'))
import warehouse_tls

ERROR, GET_STATUS = 2, 10


def read_varint(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def decode(data):
    fields, at = {}, 0
    while at < len(data):
        key, at = read_varint(data, at)
        number, kind = key >> 3, key & 7
        if kind == 0:
            value, at = read_varint(data, at)
        elif kind == 2:
            size, at = read_varint(data, at)
            value, at = data[at:at + size], at + size
        else:
            raise AssertionError(f'unexpected protobuf wire type {kind}')
        fields.setdefault(number, []).append(value)
    return fields


def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.recv(size - len(result))
        if not part:
            return bytes(result)
        result.extend(part)
    return bytes(result)


def get_status(tls, port):
    # Request(id=1, get_status={})
    request = b'\x08\x01\x52\x00'
    with warehouse_tls.connect(tls, port, 'reader') as channel:
        channel.sendall(len(request).to_bytes(4, 'big') + request)
        header = read_exact(channel, 4)
        assert len(header) == 4, 'Market closed before status reply'
        payload = read_exact(channel, int.from_bytes(header, 'big'))
    response = decode(payload)
    assert ERROR not in response, response
    assert GET_STATUS in response, response
    return decode(response[GET_STATUS][0])


def assert_stranger_refused(tls, port):
    try:
        with warehouse_tls.connect(tls, port, 'stranger') as channel:
            request = b'\x08\x01\x52\x00'
            channel.sendall(len(request).to_bytes(4, 'big') + request)
            assert read_exact(channel, 4) == b''
    except (ssl.SSLError, ConnectionError, TimeoutError):
        return
    raise AssertionError('an unlisted client certificate reached Market')


def unpack(bundle, root):
    payload = Path(bundle).read_bytes().split(b'\n__ARCHIVE__\n', 1)[1]
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
        files = {item.name for item in archive if item.isfile()}
        assert files == {
            'asterion-market/bin/asterion_market',
            'asterion-market/bin/asterion_crash_reporter',
            'asterion-market/bin/setup.sh',
            'asterion-market/bin/cold_backup.py',
            'asterion-market/config/logging.json',
            'asterion-market/lib/ctp-md.so',
            'asterion-market/lib/ctp-trader.so',
        }, files
        archive.extractall(root, filter='data')
    root = root / 'asterion-market'
    default = Path(__file__).parents[2] / 'apps/services/market/config/logging.json'
    assert (root / 'config/logging.json').read_bytes() == default.read_bytes()
    return root


def run_place_programs(bundle_dir, installed_dir):
    """Run the real setup.sh program replacement inside stock ubuntu:24.04 without Python."""
    subprocess.run(
        [
            'docker', 'run', '--rm', '--platform', 'linux/amd64', '--network', 'none',
            '--read-only', '--tmpfs', '/tmp', '--user', f'{os.getuid()}:{os.getgid()}',
            '-v', f'{bundle_dir}:/stage:ro',
            '-v', f'{installed_dir}:/market',
            'ubuntu:24.04', 'bash', '-ec',
            'if command -v python || command -v python3; then exit 1; fi; '
            'source /stage/bin/setup.sh; ROOT=/market; SHORTCUT_DIR=/tmp/bin; '
            'mkdir -p "$SHORTCUT_DIR"; place_programs /stage; rm -f "$ROOT/.program-incomplete"',
        ],
        check=True,
        timeout=60,
    )


@contextmanager
def running(root, port):
    name = 'asterion-market-package-' + root.parent.name
    command = [
        'docker', 'run', '--rm', '--platform', 'linux/amd64', '--name', name,
        '--network', 'host', '--read-only', '--tmpfs', '/tmp',
        '--user', f'{os.getuid()}:{os.getgid()}',
        '-v', f'{root}:/market',
        '-e', 'CREDENTIALS_DIRECTORY=/market/credentials',
        'ubuntu:24.04', 'sh', '-ec',
        'if command -v python || command -v python3; then exit 1; fi; '
        '/market/bin/asterion_market --version; '
        'exec /market/bin/asterion_market',
    ]
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


def unused_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def main():
    with tempfile.TemporaryDirectory(prefix='ast-market-package-') as folder:
        folder = Path(folder)
        staged = unpack(sys.argv[1], folder / 'stage')
        root = folder / 'installed'
        for name in ('config', 'credentials', 'data', 'logs'):
            (root / name).mkdir(parents=True)
        (root / 'config/logging.json').write_bytes((staged / 'config/logging.json').read_bytes())
        (root / 'data/preserved').write_text('market-state-preserved')
        (root / 'credentials/asterion.ctp.password').write_text('test-only')
        (root / 'credentials/asterion.ctp.auth_code').write_text('test-only')
        tls = warehouse_tls.prepare(root)
        port = unused_port()
        closed_front = unused_port()
        (root / 'config/market.json').write_text(json.dumps(dict(
            bind='127.0.0.1',
            port=port,
            data='/market/data',
            counter=dict(
                market_front=f'tcp://127.0.0.1:{closed_front}',
                trade_front=f'tcp://127.0.0.1:{closed_front}',
                broker='9999',
                user='000001',
                app_id='test_app',
            ),
            clients=warehouse_tls.clients(tls),
        )))
        run_place_programs(staged, root)
        assert not (root / '.program-incomplete').exists()
        with running(root, port):
            status = get_status(tls, port)
            version = status[1][0].decode()
            protocol = status[9][0].decode()
            assert version and protocol, status
            assert_stranger_refused(tls, port)
        # Re-running the installer replaces programs cleanly while preserving config and data.
        run_place_programs(staged, root)
        assert (root / 'data/preserved').read_text() == 'market-state-preserved'
        assert not (root / '.program-incomplete').exists()
        with running(root, port):
            restarted = get_status(tls, port)
            assert restarted[1][0].decode() == version
            assert restarted[9][0].decode() == protocol
    print('Pure C++ Market release: offline startup, mTLS status, rejection and upgrade passed without Python')


if __name__ == '__main__':
    main()
