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
from types import SimpleNamespace
from asterion import common_pb2 as common, strategy_pb2 as strategy, trading_pb2 as trading
from asterion.lab_pb2 import BuildPackage
from asterion_lab.package import build as build_package
from trading_installation import install, Programs


def terminal_call(process, method, params=None):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + '\n')
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    assert 'error' not in reply, reply
    return reply['result']


def rpc(host, call, value, expected_error=False):
    """The result of one Trading call, or its error with the code's short name."""
    response = account_rpc(host, trading.Request(**{call: value}), expected_error)
    if expected_error:
        code = common.ErrorCode.Name(response.error.code).removeprefix('ERROR_CODE_').lower()
        return SimpleNamespace(code=code, message=response.error.message)
    assert response.WhichOneof('result') == call, response
    return getattr(response, call)


def account_rpc(host, request, expected_error=False):
    request.id = 1
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


def account(host):
    return account_rpc(host, trading.Request(get_account={})).get_account


def account_snapshot(host):
    return account(host).session


def account_command(host, command, expected_error=False):
    current = account(host)
    command.account_id, command.policy_revision = current.id, current.session.policy_revision
    return account_rpc(host, trading.Request(send_command=command), expected_error)


def account_ready(host):
    until(lambda: account_rpc(host, trading.Request(get_health={})).get_health,
          lambda health: health.ready, seconds=15)
    return account_snapshot(host)


def initialize_account(host):
    # Explicit test policy initialization is independent of code publication.
    policy = trading.Policy(
        limits=dict(max_order_lots={'text': '5'}, max_gross_lots={'text': '10'}, max_working_orders=2),
        max_price_deviation={'text': '0.02'})
    policy.contracts.add(instrument=dict(exchange='SHFE', symbol='rb2610'), product='rb',
                         delivery_month='2026-10', multiplier={'text': '10'},
                         price_tick={'text': '1'}, lot_size={'text': '1'})
    account_rpc(host, trading.Request(open_account=policy))
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
        for name in ('strategy_pb2.py', 'common_pb2.py'):
            shutil.copy2(build / 'generated-python/asterion' / name, code / 'asterion')
        (host.root / 'runtime').mkdir()
        (host.root / 'runtime/python').symlink_to(Path(sys.executable).resolve().parents[1], target_is_directory=True)
        terminal = subprocess.Popen([str(build / 'asterion_terminal_dev_bridge')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        process = None
        try:
            fingerprint = terminal_call(terminal, 'runtime.snapshot')['client_fingerprint']
            port = host.account('account', terminals=[fingerprint, host.terminal], user='strategy')
            process = host.start('account')
            terminal_call(terminal, 'trading.connections.save', dict(id='remote',
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
            (repo / 'strategies').mkdir()
            (repo / 'strategies/strategy.py').write_text(source_code)
            (repo / 'helper.py').write_text('DEFAULT = 2\n')
            (repo / 'not-published.txt').write_text('test-only data excluded from the package')
            package = build_package(BuildPackage(repository=str(repo), strategy='strategy.TestStrategy'))
            assert [f.path for f in package.files] == ['helper.py', 'strategies/strategy.py']
            name, version = package.manifest.name, package.manifest.version
            # Real Terminal native command, generated JSON and pinned mutual TLS.
            published = terminal_call(terminal, 'trading.call', dict(account='remote',
                request=MessageToDict(trading.Request(publish_strategy=package))))['trading']['publishStrategy']
            assert published['packages'][0]['name'] == name
            assert rpc(host, 'publish_strategy', package, True).code == 'conflict'
            invalid = strategy.Package(); invalid.CopyFrom(package)
            invalid.manifest.version = 'invalid'
            invalid.manifest.files[0] = '../escape.py'; invalid.files[0].path = '../escape.py'
            assert rpc(host, 'publish_strategy', invalid, True).code == 'invalid_request'
            assert not (host.root / 'data/account/strategies/packages/strategy.TestStrategy/invalid').exists()
            invalid.CopyFrom(package); invalid.manifest.version = 'runtime'; invalid.manifest.runtime = 'unknown'
            assert rpc(host, 'publish_strategy', invalid, True).code == 'invalid_request'
            invalid.CopyFrom(package); invalid.manifest.name = '..'; invalid.manifest.version = 'escape'
            assert rpc(host, 'publish_strategy', invalid, True).code == 'invalid_request'
            assert not (host.root / 'data/account/strategies/escape').exists()
            # Repository deletion proves the instance reads installed code only.
            shutil.rmtree(repo)
            config = strategy.Instance(id='first', package_name=name, package_version=version,
                                       instruments=[dict(exchange='SHFE', symbol='rb2610')],
                                       period=common.PERIOD_MINUTES_1,
                                       params={'lots': strategy.ParameterValue(integer=3)})
            assert rpc(host, 'start_strategy', config, True).code == 'unavailable'
            assert not (host.root / 'data/account/journal.sqlite').exists()
            initialized = initialize_account(host)
            assert not initialized.HasField('authorized_ms')
            incomplete = strategy.Instance(); incomplete.CopyFrom(config)
            incomplete.weight_limits.gross.text = '1'
            assert rpc(host, 'start_strategy', incomplete, True).code == 'invalid_request'
            incomplete.weight_limits.net.text = '-1'
            assert rpc(host, 'start_strategy', incomplete, True).code == 'invalid_request'
            # Explicit zero is a valid budget; absence is not a zero default.
            config.weight_limits.gross.text = config.weight_limits.net.text = '0'
            reply = terminal_call(terminal, 'trading.call', dict(account='remote',
                request=MessageToDict(trading.Request(start_strategy=config))))['trading']['startStrategy']
            loaded = ParseDict(reply, trading.Strategies()).instance
            assert loaded.state == trading.INSTANCE_STATE_WARMING and loaded.process_id and not loaded.completed_steps
            child = loaded.process_id
            manual = trading.Command(request_id='manual.while.warming', submit_order=trading.SubmitOrder(
                order_id='manual', instrument=dict(exchange='SHFE', symbol='rb2610'),
                side=common.SIDE_BUY, offset=common.OFFSET_OPEN,
                lots={'text': '1'}, price={'text': '3500'}))
            refused = account_command(host, manual, True).error
            assert refused.code == common.ERROR_CODE_CONFLICT and 'strategy' in refused.message
            assert not account_snapshot(host).HasField('authorized_ms')
            account_command(host, trading.Command(request_id='authorize.first',
                authorize=trading.Authorize(user_id='strategy')))
            assert rpc(host, 'start_strategy', config, True).code == 'conflict'
            # Closing the entire Terminal leaves Trading and its strategy alive.
            terminal.communicate(timeout=20)
            assert terminal.returncode == 0
            time.sleep(.3)
            assert process.poll() is None and alive(child)
            assert rpc(host, 'get_strategies', {}).instance == loaded
            assert account_snapshot(host).HasField('authorized_ms')
            # Strategy death is recorded, never automatically restarted.
            os.kill(child, signal.SIGKILL)
            failed = until(lambda: rpc(host, 'get_strategies', {}).instance,
                           lambda item: item.state == trading.INSTANCE_STATE_FAILED)
            assert not failed.process_id
            time.sleep(.2)
            assert rpc(host, 'get_strategies', {}).instance == failed
            config.id = 'bad'; config.params['lots'].integer = -1
            error = rpc(host, 'start_strategy', config, True)
            assert 'fixture constructor failure' in error.message
            assert rpc(host, 'get_strategies', {}).instance.state == trading.INSTANCE_STATE_FAILED
            if sys.platform == 'linux':
                config.id = 'memory'; config.params['lots'].integer = -3
                assert 'MemoryError' in rpc(host, 'start_strategy', config, True).message
                assert process.poll() is None
                assert rpc(host, 'get_strategies', {}).instance.state == trading.INSTANCE_STATE_FAILED
            config.id = 'restart'; config.params['lots'].integer = 2
            running = rpc(host, 'start_strategy', config).instance
            process.kill(); process.communicate(timeout=10)
            until(lambda: alive(running.process_id), lambda value: not value)
            process = host.start('account')
            account_ready(host)
            recovered = rpc(host, 'get_strategies', {})
            assert recovered.instance.state == trading.INSTANCE_STATE_INTERRUPTED and not recovered.instance.process_id
            assert len(recovered.packages) == 1
            config.id = 'stopped'
            running = rpc(host, 'start_strategy', config).instance
            assert rpc(host, 'stop_strategy', {}, True).code == 'invalid_request'
            assert rpc(host, 'stop_strategy', dict(id='restart'), True).code == 'conflict'
            assert alive(running.process_id)
            stopped = rpc(host, 'stop_strategy', dict(id=config.id)).instance
            assert stopped.state == trading.INSTANCE_STATE_STOPPED and not stopped.process_id
            assert not alive(running.process_id)
            manual.request_id = 'manual.after.stop'
            assert 'authorize' in account_command(host, manual, True).error.message
            # Account faults stop a warming child without a client issuing strategy.stop.
            config.id = 'revoked'
            running = rpc(host, 'start_strategy', config).instance
            assert rpc(host, 'stop_strategy', dict(id='stopped'), True).code == 'conflict'
            assert alive(running.process_id)
            account_command(host, trading.Command(request_id='authorize.revoked',
                authorize=trading.Authorize(user_id='strategy')))
            account_command(host, trading.Command(request_id='revoke', revoke={}))
            failed = until(lambda: rpc(host, 'get_strategies', {}).instance,
                           lambda item: item.state == trading.INSTANCE_STATE_FAILED)
            assert failed.error == 'account authorization ended' and not alive(running.process_id)
            config.id = 'disconnected'
            running = rpc(host, 'start_strategy', config).instance
            account_rpc(host, trading.Request(disconnect_broker={}))
            failed = until(lambda: rpc(host, 'get_strategies', {}).instance,
                           lambda item: item.state == trading.INSTANCE_STATE_FAILED)
            assert 'connection' in failed.error and not alive(running.process_id)
            account_rpc(host, trading.Request(connect_broker={}))
            account_ready(host)
            assert rpc(host, 'get_strategies', {}).instance.state == trading.INSTANCE_STATE_FAILED
            if sys.platform == 'linux':
                config.id = 'timeout'; config.params['lots'].integer = -2
                started = time.monotonic()
                rpc(host, 'start_strategy', config, True)
                assert 9 <= time.monotonic() - started < 16
                timed_out = rpc(host, 'get_strategies', {}).instance
                assert timed_out.state == trading.INSTANCE_STATE_FAILED and not timed_out.process_id
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
                    send(loading, trading.Request(id=1, start_strategy=config))
                    def child_pid():
                        result = subprocess.run(['pgrep', '-P', str(process.pid)], capture_output=True, text=True)
                        return int(result.stdout.split()[0]) if result.stdout.strip() else 0
                    hung = until(child_pid, bool)
                    arguments = Path(f'/proc/{hung}/cmdline').read_bytes().split(b'\0')
                    endpoint = Path(os.fsdecode(arguments[-4]))
                    until(lambda: endpoint.with_name('constructor').exists(), bool)
                    for i in range(12):
                        queued = connections.enter_context(host.client('account'))
                        send(queued, trading.Request(id=i + 2, get_strategies={}))
                    with host.client('account') as health:
                        send(health, trading.Request(id=99, get_health={}))
                        header = health.recv(4)
                        assert len(header) == 4
                        size, = struct.unpack('!I', header)
                        raw = bytearray()
                        while len(raw) < size:
                            raw.extend(health.recv(size - len(raw)))
                        assert trading.Response.FromString(raw).HasField('get_health')
                    process.kill(); process.communicate(timeout=10)
                    until(lambda: alive(hung), lambda value: not value)
                process = host.start('account')
                assert rpc(host, 'get_strategies', {}).instance.state == trading.INSTANCE_STATE_INTERRUPTED
            account_ready(host)
            assert not account_snapshot(host).HasField('authorized_ms')
            assert not account_snapshot(host).orders
            config.id = 'graceful'; config.params['lots'].integer = 2
            running = rpc(host, 'start_strategy', config).instance
            terminate(process)
            assert process.returncode == 0 and not alive(running.process_id)
            print('Terminal publication, account ownership, isolated loading, independent lifetime and restart interruption passed')
        finally:
            terminate(terminal)
            terminate(process)


if __name__ == '__main__':
    main()
