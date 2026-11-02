"""Terminal publishes code; an independent C++ Trading owns its Python child.

Only test credentials and a test SDK are used. No provider is contacted.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [str(source / 'tests/support'), str(source / 'apps/services/lab'),
               str(source / 'apps/services/common/python'), str(build / 'generated-python')]
from google.protobuf.json_format import MessageToDict, ParseDict
from asterion import strategy_pb2 as strategy, trading_pb2 as trading
from asterion_lab.package import build as build_package
from trading_installation import install, Programs


def terminal_call(process, method, params=None):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + '\n')
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    assert 'error' not in reply, reply
    return reply['result']


def rpc(host, operation, expected_error=False):
    response = account_rpc(host, trading.Request(strategy=operation), expected_error)
    if expected_error:
        return response.error
    assert response.HasField('strategy'), response
    return response.strategy


def account_rpc(host, request, expected_error=False):
    request.version, request.session_id, request.correlation_id = 1, 'account', 'strategy-test'
    with host.client('account') as channel:
        channel.settimeout(20)
        raw = request.SerializeToString()
        channel.sendall(struct.pack('!I', len(raw)) + raw)
        def exact(size):
            result = bytearray()
            while len(result) < size:
                value = channel.recv(size - len(result))
                assert value, 'Trading closed its reply'
                result.extend(value)
            return bytes(result)
        size, = struct.unpack('!I', exact(4))
        response = trading.Response.FromString(exact(size))
    if expected_error:
        assert response.HasField('error'), response
    else:
        assert not response.HasField('error'), response
    return response


def account_snapshot(host):
    return account_rpc(host, trading.Request(snapshot=trading.Empty())).live


def account_command(host, command, expected_error=False):
    current = account_snapshot(host)
    return account_rpc(host, trading.Request(account_id=current.account_id,
        policy_revision=current.policy_revision, command=command), expected_error)


def account_ready(host):
    until(lambda: account_rpc(host, trading.Request(heartbeat=trading.Empty())).health,
          lambda health: health.execution.business_ready, seconds=15)
    return account_snapshot(host)


def initialize_account(host):
    # Explicit test policy initialization is independent of code publication.
    policy = trading.LivePolicy()
    policy.risk.max_order_quantity.units = 5 * 10**8
    policy.risk.max_gross_quantity.units = 10 * 10**8
    policy.risk.max_working_orders = 2
    policy.max_price_deviation.units = 2 * 10**6
    contract = policy.contracts.add(venue='SHFE', symbol='rb2610', currency='CNY',
                                    product='rb', delivery_month='2026-10')
    contract.price_increment.units = contract.quantity_increment.units = 10**8
    contract.multiplier.units = 10 * 10**8
    account_rpc(host, trading.Request(live_create=policy))
    return account_ready(host)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # An orphan is reaped asynchronously by Linux's init.
    stat = Path(f'/proc/{pid}/stat')
    return not stat.exists() or stat.read_text().split()[2] != 'Z'


def until(operation, predicate, seconds=8):
    end = time.monotonic() + seconds
    while True:
        result = operation()
        if predicate(result):
            return result
        assert time.monotonic() < end, result
        time.sleep(.05)


def terminate(process):
    if process and process.poll() is None:
        process.terminate()
        process.communicate(timeout=20)


def main():
    with tempfile.TemporaryDirectory(prefix='ast-strategy-test-') as folder:
        root = Path(folder)
        suffix = '.dylib' if sys.platform == 'darwin' else '.so'
        host = install(root / 'trading', Programs(str(build / 'asterion_trading'),
            str(build / 'asterion_test_certificates'), str(build / f'libasterion_test_ctp_trader{suffix}')))
        code = host.root / 'lib/strategy'
        (host.root / 'lib/ctp-md.so').symlink_to(build / f'libasterion_test_ctp{suffix}')
        (code / 'asterion').mkdir(parents=True)
        shutil.copy2(source / 'apps/services/trading/python/strategy_runner.py', code)
        shutil.copy2(source / 'apps/services/common/python/asterion_strategy.py', code)
        shutil.copy2(build / 'generated-python/asterion/strategy_pb2.py', code / 'asterion')
        (host.root / 'runtime').mkdir()
        (host.root / 'runtime/python').symlink_to(Path(sys.executable).resolve().parents[1], target_is_directory=True)
        terminal = subprocess.Popen([str(build / 'asterion_terminal_dev_bridge')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        process = None
        try:
            fingerprint = terminal_call(terminal, 'trading.identity.prepare')['trading_fingerprint']
            port = host.account('account', terminals=[fingerprint, host.terminal], user='strategy')
            process = host.start('account')
            terminal_call(terminal, 'trading.connections.save', dict(id='remote', name='Test Trading',
                revision='', host='127.0.0.1', port=port, instance='account', fingerprint=host.server))
            repo = root / 'repository'
            repo.mkdir()
            subprocess.run(['git', '-C', str(repo), 'init', '-q'], check=True)
            # Package preparation needs no Git commit and never executes code.
            source_code = '''from asterion_strategy import Strategy
from helper import DEFAULT
import sys
assert not any(name.startswith('asterion_lab') for name in sys.modules)
class TestStrategy(Strategy):
    params = {"lots": DEFAULT}
    def ready(self, ctx):
        return all(len(ctx.history(symbol, "close", 2)) == 2 for symbol in ctx.symbols)
    def __init__(self, **params):
        super().__init__(**params)
        if self.params["lots"] == -1:
            raise ValueError("fixture constructor failure")
        if self.params["lots"] == -2:
            from pathlib import Path
            Path(sys.argv[1]).with_name('constructor').write_text('entered')
            while True:
                pass
        if self.params["lots"] == -3:
            bytearray(2 * 1024 ** 3)
    def on_bar(self, ctx, bar):
        ctx.target(bar.symbol, self.params["lots"])
'''
            (repo / 'strategy.py').write_text(source_code)
            (repo / 'helper.py').write_text('DEFAULT = 2\n')
            (repo / 'not-published.txt').write_text('test-only data excluded from the package')
            manifest = dict(name='example', version='1', entry='strategy:TestStrategy',
                            runtime='python3.12-numpy2.3.5', files=['strategy.py', 'helper.py'])
            (repo / 'strategy.json').write_text(json.dumps(manifest))
            package = build_package(strategy.Build(repository=str(repo), manifest='strategy.json'))
            assert [f.path for f in package.files] == manifest['files']
            # Real Terminal native command, generated JSON and pinned mutual TLS.
            published = terminal_call(terminal, 'trading.call', dict(account='remote',
                request=MessageToDict(trading.Request(strategy=strategy.Request(publish=package)))))['trading']['strategy']
            assert published['packages'][0]['name'] == 'example'
            assert rpc(host, strategy.Request(publish=package), True).code == 'conflict'
            invalid = strategy.Package(); invalid.CopyFrom(package)
            invalid.manifest.version = 'invalid'
            invalid.manifest.files[0] = '../escape.py'; invalid.files[0].path = '../escape.py'
            assert rpc(host, strategy.Request(publish=invalid), True).code == 'invalid_request'
            assert not (host.root / 'data/account/strategies/packages/example/invalid').exists()
            invalid.CopyFrom(package); invalid.manifest.version = 'runtime'; invalid.manifest.runtime = 'unknown'
            assert rpc(host, strategy.Request(publish=invalid), True).code == 'invalid_request'
            invalid.CopyFrom(package); invalid.manifest.name = '..'; invalid.manifest.version = 'escape'
            assert rpc(host, strategy.Request(publish=invalid), True).code == 'invalid_request'
            assert not (host.root / 'data/account/strategies/escape').exists()
            # Repository deletion proves the instance reads installed code only.
            shutil.rmtree(repo)
            config = strategy.Instance(id='first', package=strategy.PackageRef(name='example', version='1'),
                                       contracts=['SHFE/rb2610'], period='1m',
                                       params={'lots': strategy.ParameterValue(integer=3)})
            assert rpc(host, strategy.Request(start=config), True).code == 'unavailable'
            assert not (host.root / 'data/account/journal.sqlite').exists()
            initialized = initialize_account(host)
            assert not initialized.HasField('authorization')
            incomplete = strategy.Instance(); incomplete.CopyFrom(config)
            incomplete.weight_budget.gross_units = 10**8
            assert rpc(host, strategy.Request(start=incomplete), True).code == 'invalid_request'
            incomplete.weight_budget.net_units = -1
            assert rpc(host, strategy.Request(start=incomplete), True).code == 'invalid_request'
            # Explicit zero is a valid budget; absence is not a zero default.
            config.weight_budget.gross_units = config.weight_budget.net_units = 0
            reply = terminal_call(terminal, 'trading.call', dict(account='remote',
                request=MessageToDict(trading.Request(strategy=strategy.Request(start=config)))))['trading']['strategy']
            loaded = ParseDict(reply, strategy.State()).instance
            assert loaded.state == 'warming' and loaded.process_id and not loaded.completed_steps
            child = loaded.process_id
            manual = trading.Command(request_id='manual.while.warming', submit=trading.Submit(
                order_id='manual', venue='SHFE', symbol='rb2610', side=trading.BUY, offset=trading.OPEN))
            manual.submit.price.units = 3500 * 10**8
            manual.submit.quantity.units = 10**8
            refused = account_command(host, manual, True).error
            assert refused.code == 'conflict' and 'strategy' in refused.message
            assert not account_snapshot(host).HasField('authorization')
            account_command(host, trading.Command(request_id='authorize.first',
                live_authorize=trading.LiveAuthorize(user_id='strategy')))
            assert rpc(host, strategy.Request(start=config), True).code == 'conflict'
            # Closing the entire Terminal leaves Trading and its strategy alive.
            terminal.communicate(timeout=20)
            assert terminal.returncode == 0
            time.sleep(.3)
            assert process.poll() is None and alive(child)
            assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance == loaded
            assert account_snapshot(host).HasField('authorization')
            # Strategy death is recorded, never automatically restarted.
            os.kill(child, signal.SIGKILL)
            failed = until(lambda: rpc(host, strategy.Request(inspect=strategy.Empty())).instance,
                           lambda item: item.state == 'failed')
            assert not failed.process_id
            time.sleep(.2)
            assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance == failed
            config.id = 'bad'; config.params['lots'].integer = -1
            error = rpc(host, strategy.Request(start=config), True)
            assert 'fixture constructor failure' in error.message
            assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance.state == 'failed'
            if sys.platform == 'linux':
                config.id = 'memory'; config.params['lots'].integer = -3
                assert 'MemoryError' in rpc(host, strategy.Request(start=config), True).message
                assert process.poll() is None
                assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance.state == 'failed'
            config.id = 'restart'; config.params['lots'].integer = 2
            running = rpc(host, strategy.Request(start=config)).instance
            process.kill(); process.communicate(timeout=10)
            until(lambda: alive(running.process_id), lambda value: not value)
            process = host.start('account')
            account_ready(host)
            recovered = rpc(host, strategy.Request(inspect=strategy.Empty()))
            assert recovered.instance.state == 'interrupted' and not recovered.instance.process_id
            assert len(recovered.packages) == 1
            config.id = 'stopped'
            running = rpc(host, strategy.Request(start=config)).instance
            assert rpc(host, strategy.Request(stop=strategy.Stop()), True).code == 'invalid_request'
            assert rpc(host, strategy.Request(stop=strategy.Stop(instance_id='restart')), True).code == 'conflict'
            assert alive(running.process_id)
            stopped = rpc(host, strategy.Request(stop=strategy.Stop(instance_id=config.id))).instance
            assert stopped.state == 'stopped' and not stopped.process_id
            assert not alive(running.process_id)
            manual.request_id = 'manual.after.stop'
            assert 'authorize' in account_command(host, manual, True).error.message
            # Account faults stop a warming child without a client issuing strategy.stop.
            config.id = 'revoked'
            running = rpc(host, strategy.Request(start=config)).instance
            assert rpc(host, strategy.Request(stop=strategy.Stop(instance_id='stopped')), True).code == 'conflict'
            assert alive(running.process_id)
            account_command(host, trading.Command(request_id='authorize.revoked',
                live_authorize=trading.LiveAuthorize(user_id='strategy')))
            account_command(host, trading.Command(request_id='revoke', live_revoke=trading.Empty()))
            failed = until(lambda: rpc(host, strategy.Request(inspect=strategy.Empty())).instance,
                           lambda item: item.state == 'failed')
            assert failed.error == 'account authorization ended' and not alive(running.process_id)
            config.id = 'disconnected'
            running = rpc(host, strategy.Request(start=config)).instance
            account_rpc(host, trading.Request(live_disconnect=trading.Empty()))
            failed = until(lambda: rpc(host, strategy.Request(inspect=strategy.Empty())).instance,
                           lambda item: item.state == 'failed')
            assert 'connection' in failed.error and not alive(running.process_id)
            account_rpc(host, trading.Request(live_connect=trading.Empty()))
            account_ready(host)
            assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance.state == 'failed'
            if sys.platform == 'linux':
                config.id = 'timeout'; config.params['lots'].integer = -2
                started = time.monotonic()
                rpc(host, strategy.Request(start=config), True)
                assert 9 <= time.monotonic() - started < 16
                timed_out = rpc(host, strategy.Request(inspect=strategy.Empty())).instance
                assert timed_out.state == 'failed' and not timed_out.process_id
                assert process.poll() is None
                # A hung constructor cannot block the account's public I/O or
                # survive its owner, even with the administration queue full.
                from contextlib import ExitStack
                config.id = 'hung'; config.params['lots'].integer = -2
                def send(channel, request):
                    raw = request.SerializeToString()
                    channel.sendall(struct.pack('!I', len(raw)) + raw)
                with ExitStack() as connections:
                    loading = connections.enter_context(host.client('account'))
                    send(loading, trading.Request(version=1, session_id='account', correlation_id='hung',
                         strategy=strategy.Request(start=config)))
                    def child_pid():
                        result = subprocess.run(['pgrep', '-P', str(process.pid)], capture_output=True, text=True)
                        return int(result.stdout.split()[0]) if result.stdout.strip() else 0
                    hung = until(child_pid, bool)
                    arguments = Path(f'/proc/{hung}/cmdline').read_bytes().split(b'\0')
                    endpoint = Path(os.fsdecode(arguments[-4]))
                    until(lambda: endpoint.with_name('constructor').exists(), bool)
                    for i in range(12):
                        queued = connections.enter_context(host.client('account'))
                        send(queued, trading.Request(version=1, session_id='account', correlation_id=f'queue-{i}',
                             strategy=strategy.Request(inspect=strategy.Empty())))
                    with host.client('account') as health:
                        send(health, trading.Request(version=1, session_id='account', correlation_id='health',
                             heartbeat=trading.Empty()))
                        header = health.recv(4)
                        assert len(header) == 4
                        size, = struct.unpack('!I', header)
                        raw = bytearray()
                        while len(raw) < size:
                            raw.extend(health.recv(size - len(raw)))
                        assert trading.Response.FromString(raw).HasField('health')
                    process.kill(); process.communicate(timeout=10)
                    until(lambda: alive(hung), lambda value: not value)
                process = host.start('account')
                assert rpc(host, strategy.Request(inspect=strategy.Empty())).instance.state == 'interrupted'
            account_ready(host)
            assert not account_snapshot(host).HasField('authorization')
            assert not account_snapshot(host).orders
            config.id = 'graceful'; config.params['lots'].integer = 2
            running = rpc(host, strategy.Request(start=config)).instance
            terminate(process)
            assert process.returncode == 0 and not alive(running.process_id)
            print('Terminal publication, account ownership, isolated loading, independent lifetime and restart interruption passed')
        finally:
            terminate(terminal)
            terminate(process)


if __name__ == '__main__':
    main()
