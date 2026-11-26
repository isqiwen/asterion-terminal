"""Strategy children enforce resource limits while running NumPy and SciPy."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def probe(source, build):
    paths = [str(source / 'apps/services/trading/python'),
             str(source / 'apps/services/common/python'), str(build / 'generated-python')]
    sys.path[:0] = paths
    from asterion import strategy_pb2 as wire
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        package = folder / 'package'
        package.mkdir()
        (package / 'strategy.json').write_text(json.dumps({'entry': 'strategy:Test'}))
        (package / 'strategy.py').write_text('''from asterion_strategy import Strategy
import os, resource
from pathlib import Path
class Test(Strategy):
    params = {"memory": False, "expected_mb": 2048}
    def __init__(self, **params):
        super().__init__(**params)
        assert 'CREDENTIALS_DIRECTORY' not in os.environ
        expected_bytes = self.params['expected_mb'] * 1024 ** 2
        assert resource.getrlimit(resource.RLIMIT_AS) == (expected_bytes,) * 2
        assert os.getpriority(os.PRIO_PROCESS, 0) >= 10
        assert int(Path('/proc/self/oom_score_adj').read_text().strip()) >= 500
        if self.params['memory']:
            bytearray(expected_bytes + 1024 ** 3)
        try:
            resource.setrlimit(resource.RLIMIT_AS, (expected_bytes + 1024 ** 3,) * 2)
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
        import scipy.stats
        assert np.dot(np.ones((32, 32)), np.ones((32, 32)))[0, 0] == 32
        assert list(scipy.stats.rankdata([30.0, 10.0, 20.0])) == [3.0, 1.0, 2.0]
    def ready(self, ctx):
        return len(ctx.history(ctx.symbols[0], 'close', 2)) == 2
    def on_bar(self, ctx, bar):
        ctx.target(bar.symbol, 3)
''')
        for limit_arg, expected_mb, memory in ((None, 2048, False), (None, 2048, True),
                                               ('1024', 1024, False), ('1024', 1024, True)):
            endpoint = folder / 'child.sock'
            endpoint.unlink(missing_ok=True)
            bootstrap = f'import sys; sys.path[:0] = {paths!r}; import strategy_runner; strategy_runner.main()'
            argv = [sys.executable, '-I', '-B', '-c', bootstrap,
                    str(endpoint), str(package), str(os.getpid())]
            if limit_arg is not None:
                argv.append(limit_arg)
            child = subprocess.Popen(argv,
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
                        return wire.Response.FromString(exact(size))
                    config = wire.Instance(instruments=[dict(exchange='SHFE', symbol='rb2610')],
                        params={'memory': wire.ParameterValue(flag=memory),
                                'expected_mb': wire.ParameterValue(integer=expected_mb)})
                    reply = exchange(wire.Request(load=wire.Load(instance=config,
                                                                 symbols=['SHFE/rb/2026-10'])))
                    if memory:
                        assert 'MemoryError' in reply.error.message, reply
                    else:
                        assert reply.HasField('load'), reply
                        # The native numeric kernel remained single-threaded.
                        assert len(list(Path(f'/proc/{child.pid}/task').iterdir())) == 1
                        for seq in (1, 2):
                            request = wire.Request()
                            request.bars.sequence = seq
                            price = {'text': '3000'}
                            request.bars.bars.add(instrument=dict(exchange='SHFE', symbol='rb2610'),
                                end_ms=seq * 60000, open=price, high=price, low=price, close=price,
                                volume=1, open_interest={'text': '1'})
                            decision = exchange(request).bars
                            assert decision.sequence == seq and decision.ready == (seq == 2)
                            assert len(decision.targets) == (1 if seq == 2 else 0)
                            if seq == 2: assert decision.targets[0].lots == 3
                assert child.wait(timeout=5) == 0
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
    print('Strategy warmup, targets and child resource limits passed')


if __name__ == '__main__':
    probe(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
