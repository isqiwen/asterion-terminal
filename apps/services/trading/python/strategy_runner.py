"""Trading-owned strategy child. Loads one immutable package and instance.

The private socket is its lifetime: Trading's exit closes it, including SIGKILL.
There is deliberately no historical loader or order interface. Only complete
bars observed by this Trading instance enter its initially empty context.
"""
import importlib
import json
import os
from pathlib import Path
import socket
import struct
import sys
import traceback
import datetime
from decimal import Decimal, ROUND_HALF_EVEN

# Bind lifetime before importing strategy code, including a hung constructor.
if sys.platform == 'linux':
    import ctypes
    import resource
    import signal
    if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
        raise OSError('cannot bind strategy lifetime to Trading')
    if os.getppid() != int(sys.argv[3]):
        sys.exit(1)
    # Hard limits apply before NumPy or any package code is imported. No
    # lifetime CPU limit: a healthy strategy may run for months; Trading owns
    # the 10 s load and 2 s per-input deadlines instead.
    for kind, limit in ((resource.RLIMIT_AS, 1024 ** 3),
                        (resource.RLIMIT_NOFILE, 64),
                        (resource.RLIMIT_FSIZE, 16 * 1024 ** 2),
                        (resource.RLIMIT_CORE, 0), (resource.RLIMIT_NPROC, 0)):
        _, inherited = resource.getrlimit(kind)
        if inherited != resource.RLIM_INFINITY:
            limit = min(limit, inherited)
        resource.setrlimit(kind, (limit, limit))
    os.nice(10)
os.environ.pop('CREDENTIALS_DIRECTORY', None)
# Numeric kernels must not create a CPU-sized thread pool in an account child.
for name in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS',
             'VECLIB_MAXIMUM_THREADS', 'NUMEXPR_NUM_THREADS'):
    os.environ[name] = '1'

import numpy
from asterion.common_pb2 import ErrorCode
from asterion.strategy_pb2 import Decision, Request, Response
from asterion_strategy import Strategy, Context, Bar, step


def receive(channel):
    def exact(size):
        value = bytearray()
        while len(value) < size:
            part = channel.recv(size - len(value))
            if not part:
                raise EOFError
            value.extend(part)
        return bytes(value)
    size, = struct.unpack('!I', exact(4))
    if not 0 < size <= 65536:
        raise ValueError('invalid strategy frame size')
    return Request.FromString(exact(size))


def reply(channel, value):
    value = value.SerializeToString()
    channel.sendall(struct.pack('!I', len(value)) + value)


def main():
    endpoint, package = sys.argv[1:3]
    os.umask(0o077)
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(endpoint)
        listener.listen(1)
        with listener.accept()[0] as channel:
            try:
                request = receive(channel)
                if request.WhichOneof('call') != 'load':
                    raise ValueError('first strategy request must load an instance')
                if sys.version_info[:2] != (3, 12) or numpy.__version__ != '2.3.5':
                    raise ValueError('strategy requires Python 3.12 and NumPy 2.3.5')
                manifest = json.loads((Path(package) / 'strategy.json').read_text())
                module, name = manifest['entry'].split(':')
                sys.path.insert(0, package)
                cls = getattr(importlib.import_module(module), name)
                if not isinstance(cls, type) or not issubclass(cls, Strategy) or cls is Strategy:
                    raise ValueError('package entry must be a Strategy subclass')
                if cls.ready is Strategy.ready:
                    raise ValueError('a live strategy must explicitly report readiness')
                params = {}
                for key, value in request.load.params.items():
                    field = value.WhichOneof('value')
                    if field is None:
                        raise ValueError(f'missing parameter value: {key}')
                    params[key] = getattr(value, field)
                strategy = cls(**params)
                context = Context([f'{i.exchange}/{i.symbol}' for i in request.load.instruments])
                reply(channel, Response(load={}))
                sequence = 0
                while True:
                    request = receive(channel)
                    operation = request.WhichOneof('call')
                    if operation == 'stop':
                        break
                    if operation != 'bars' or request.bars.sequence != sequence + 1:
                        raise ValueError('strategy inputs must arrive once in sequence')
                    bars = [Bar(f'{row.instrument.exchange}/{row.instrument.symbol}',
                        f'{row.instrument.exchange}/{row.instrument.symbol}',
                        datetime.datetime.fromtimestamp(row.end_ms / 1000,
                            datetime.timezone(datetime.timedelta(hours=8))),
                        float(row.open.text), float(row.high.text),
                        float(row.low.text), float(row.close.text),
                        row.volume, float(row.open_interest.text))
                        for row in request.bars.bars]
                    wanted = step(strategy, context, bars)
                    sequence = request.bars.sequence
                    decision = Decision(sequence=sequence, ready=context.ready)
                    for contract, value in wanted.items():
                        exchange, symbol = contract.split('/')
                        target = decision.targets.add(instrument=dict(exchange=exchange, symbol=symbol))
                        if context.mode == 'target':
                            target.lots = value
                        else:
                            target.weight.text = str(Decimal(str(value)).quantize(
                                Decimal('0.00000001'), rounding=ROUND_HALF_EVEN).normalize())
                    reply(channel, Response(bars=decision))
                del strategy, context
            except EOFError:
                return
            except Exception as error:
                traceback.print_exc()
                reply(channel, Response(error=dict(code=ErrorCode.ERROR_CODE_OPERATION_FAILED,
                    message=f'{type(error).__name__}: {error}'[:4096])))


if __name__ == '__main__':
    main()
