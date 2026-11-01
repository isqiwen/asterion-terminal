"""Exercise the actual release offline in stock Ubuntu, without host Python.

The container runs as a non-root service user. All bars and package code below
are test fixtures; no account is configured and no provider is contacted.
"""
import io
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import time


def probe(root):
    import importlib.metadata
    assert shutil.which('python3') is None, 'container must have no system Python'
    assert sys.version_info[:3] == (3, 12, 12)
    assert {p.metadata['Name'].lower() for p in importlib.metadata.distributions()} == {'numpy', 'protobuf'}
    subprocess.run([root / 'bin/asterion_trading', '--version'], check=True)
    code = root / 'lib/strategy'
    sys.path.insert(0, str(code))
    from asterion import strategy_pb2 as wire
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        package = folder / 'package'
        package.mkdir()
        (package / 'strategy.json').write_text(json.dumps({'entry': 'strategy:Test'}))
        (package / 'strategy.py').write_text('''from asterion_strategy import Strategy
import os, resource
class Test(Strategy):
    params = {"memory": False}
    def __init__(self, **params):
        super().__init__(**params)
        assert 'CREDENTIALS_DIRECTORY' not in os.environ
        assert resource.getrlimit(resource.RLIMIT_AS) == (1024 ** 3,) * 2
        assert os.getpriority(os.PRIO_PROCESS, 0) >= 10
        if self.params['memory']:
            bytearray(2 * 1024 ** 3)
        try:
            resource.setrlimit(resource.RLIMIT_AS, (2 * 1024 ** 3,) * 2)
        except ValueError:
            pass
        else:
            raise AssertionError('strategy raised its memory ceiling')
        try:
            child = os.fork()
        except BlockingIOError:
            pass
        else:
            if child == 0: os._exit(0)
            os.waitpid(child, 0)
            raise AssertionError('strategy forked')
        import threading
        try:
            thread = threading.Thread(target=lambda: None)
            thread.start()
        except RuntimeError:
            pass
        else:
            thread.join()
            raise AssertionError('strategy created a thread')
        handles = []
        try:
            while True: handles.append(os.open('/dev/null', os.O_RDONLY))
        except OSError as error:
            import errno
            assert error.errno == errno.EMFILE and len(handles) < 64
        finally:
            for handle in handles: os.close(handle)
        import tempfile, errno
        with tempfile.TemporaryFile() as file:
            try: file.truncate(17 * 1024 ** 2)
            except OSError as error: assert error.errno == errno.EFBIG
            else: raise AssertionError('strategy exceeded its output file ceiling')
        import numpy as np
        assert np.dot(np.ones((32, 32)), np.ones((32, 32)))[0, 0] == 32
    def ready(self, ctx):
        return len(ctx.history(ctx.symbols[0], 'close', 2)) == 2
    def on_bar(self, ctx, bar):
        ctx.target(bar.symbol, 3)
''')
        for memory in (False, True):
            endpoint = folder / 'child.sock'
            endpoint.unlink(missing_ok=True)
            bootstrap = f'import sys; sys.path.insert(0, {str(code)!r}); import strategy_runner; strategy_runner.main()'
            child = subprocess.Popen([sys.executable, '-I', '-B', '-c', bootstrap,
                str(endpoint), str(package), str(os.getpid())],
                env=dict(os.environ, CREDENTIALS_DIRECTORY='test-only-removed', OPENBLAS_NUM_THREADS='64'))
            try:
                deadline = time.monotonic() + 5
                while not endpoint.exists():
                    assert child.poll() is None
                    assert time.monotonic() < deadline
                    time.sleep(.01)
                with socket.socket(socket.AF_UNIX) as channel:
                    channel.settimeout(5)
                    channel.connect(str(endpoint))
                    def exchange(request):
                        value = request.SerializeToString()
                        channel.sendall(struct.pack('!I', len(value)) + value)
                        def exact(size):
                            result = bytearray()
                            while len(result) < size:
                                part = channel.recv(size - len(result))
                                assert part
                                result.extend(part)
                            return bytes(result)
                        size, = struct.unpack('!I', exact(4))
                        return wire.ChildReply.FromString(exact(size))
                    config = wire.Instance(contracts=['SHFE/rb2610'],
                        params={'memory': wire.ParameterValue(flag=memory)})
                    reply = exchange(wire.ChildRequest(load=config))
                    if memory:
                        assert 'MemoryError' in reply.error, reply
                    else:
                        assert reply.loaded, reply
                        # The native numeric kernel remained single-threaded.
                        assert len(list(Path(f'/proc/{child.pid}/task').iterdir())) == 1
                        for seq in (1, 2):
                            request = wire.ChildRequest()
                            request.bars.sequence = seq
                            request.bars.bars.add(contract='SHFE/rb2610', end_ms=seq * 60000,
                                open_units=300000000000, high_units=300000000000,
                                low_units=300000000000, close_units=300000000000,
                                volume=1, open_interest_units=100000000)
                            decision = exchange(request).decision
                            assert decision.sequence == seq and decision.ready == (seq == 2)
                            assert len(decision.targets) == (1 if seq == 2 else 0)
                            if seq == 2: assert decision.targets[0].lots == 3
                assert child.wait(timeout=5) == 0
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
    print('Offline release: relocated runtime, package warmup, targets and child resource limits passed')


def main():
    with tempfile.TemporaryDirectory(prefix='ast-trading-package-') as folder:
        folder = Path(folder)
        payload = Path(sys.argv[1]).read_bytes().split(b'\n__ARCHIVE__\n', 1)[1]
        with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
            archive.extractall(folder, filter='data')
        root = folder / 'asterion-trading'
        assert {p.name for p in root.iterdir()} == {'bin', 'lib', 'runtime'}
        assert not list(root.rglob('*asterion_lab*'))
        assert not list(root.rglob('pip'))
        subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--read-only',
            '--tmpfs', '/tmp:rw,exec,nosuid,size=1g', '--user', f'{os.getuid()}:{os.getgid()}',
            '-v', f'{root}:/trading:ro', '-v', f'{Path(__file__).resolve()}:/probe.py:ro',
            'ubuntu:24.04', '/trading/runtime/python/bin/python3', '-I', '-B',
            '/probe.py', '--probe', '/trading'], check=True, timeout=60)


if __name__ == '__main__':
    if sys.argv[1] == '--probe':
        # Use the real installer's program replacement in a disposable root.
        # systemd admission is tested separately; no system service is changed.
        with tempfile.TemporaryDirectory() as folder:
            installed = Path(folder) / 'installed'
            installed.mkdir()
            (installed / '.program.lock').touch()
            (installed / 'data').mkdir()
            (installed / 'data/preserved').write_text('test-only strategy state')
            for _ in range(2):
                subprocess.run(['bash', '-c',
                    'source "$1/bin/setup.sh"; ROOT=$2; write_unit() { :; }; '
                    'exec 9<"$ROOT/.program.lock"; flock --exclusive --nonblock 9; place "$1"',
                    'install', sys.argv[2], str(installed)], check=True)
                assert (installed / 'data/preserved').read_text() == 'test-only strategy state'
                assert not (installed / 'runtime/obsolete.py').exists()
                assert not (installed / '.program-incomplete').exists()
                assert os.access(installed / 'bin/setup.sh', os.X_OK)
                probe(installed)
                (installed / 'runtime/obsolete.py').touch()
    else:
        main()
