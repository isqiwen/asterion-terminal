"""Actual C++ Trading + test CTP stream + Python child, on the real clock.

Waits for two complete minute bars (at most three minutes); does not accelerate
timestamps past the production freshness check. Only an isolated test account is used.
"""
import datetime
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from strategy_process import source, build, rpc, until, terminate, initialize_account, account_snapshot, account_command
from trading_installation import install, Programs
from asterion import strategy_pb2 as wire, trading_pb2 as trading
from asterion_strategy import Bar, Context, step

CODE = '''from asterion_strategy import Strategy
class TwoBars(Strategy):
    params = {"window": 2, "lots": 3}
    def ready(self, ctx):
        return all(len(ctx.history(s, "close", self.params["window"])) == self.params["window"]
                   for s in ctx.symbols)
    def on_bar(self, ctx, bar):
        # Deliberately asks before readiness: the shared driver must discard it.
        ctx.target(bar.symbol, self.params["lots"])
'''


def main():
    with tempfile.TemporaryDirectory(prefix='ast-strategy-stream-') as directory:
        suffix = '.dylib' if sys.platform == 'darwin' else '.so'
        host = install(Path(directory) / 'trading', Programs(str(build / 'asterion_trading'),
            str(build / 'asterion_test_certificates'), str(build / f'libasterion_test_ctp_trader{suffix}')))
        (host.root / 'lib/ctp-md.so').symlink_to(build / f'libasterion_test_ctp{suffix}')
        code = host.root / 'lib/strategy'
        (code / 'asterion').mkdir(parents=True)
        for path in (source / 'apps/services/trading/python/strategy_runner.py',
                     source / 'apps/services/common/python/asterion_strategy.py'):
            shutil.copy2(path, code)
        shutil.copy2(build / 'generated-python/asterion/strategy_pb2.py', code / 'asterion')
        (host.root / 'runtime').mkdir()
        (host.root / 'runtime/python').symlink_to(Path(sys.executable).resolve().parents[1])
        host.account('account', user='strategy')
        process = host.start('account')
        root = host.root / 'data/account/strategies'
        try:
            initialize_account(host)
            account_command(host, trading.Command(request_id='authorize.stream',
                live_authorize=trading.LiveAuthorize(user_id='strategy')))
            manifest = wire.Manifest(name='two-bars', version='1', entry='rule:TwoBars',
                runtime='python3.12-numpy2.3.5', files=['rule.py'])
            rpc(host, wire.Request(publish=wire.Package(manifest=manifest,
                files=[wire.File(path='rule.py', content=CODE.encode())])))
            config = wire.Instance(id='live-window', package=wire.PackageRef(name='two-bars', version='1'),
                contracts=['SHFE/rb2610'], period='1m')
            started_ms = int(time.time() * 1000)
            initial = rpc(host, wire.Request(start=config)).instance
            assert initial.state == 'warming' and not initial.completed_steps, initial
            def observe():
                state = rpc(host, wire.Request(inspect=wire.Empty())).instance
                assert state.state in ('warming', 'ready'), state
                return state
            first = until(observe, lambda state: state.completed_steps >= 1, seconds=130)
            assert first.completed_steps == 1 and first.state == 'warming', first
            assert not first.last_decision.targets and not first.last_decision.ready
            assert not account_snapshot(host).orders
            ready = until(observe, lambda state: state.state == 'ready', seconds=70)
            assert ready.completed_steps == 2 and ready.last_decision.targets[0].lots == 3, ready
            sent = until(lambda: account_snapshot(host), lambda state: len(state.orders) == 1)
            assert sent.orders[0].quantity.units == 3 * 10**8, sent
            order_id = sent.orders[0].id
            # Loss of the direct provider stream ends this instance, never reconnects it.
            (root / 'ctp-flow/disconnect').touch()
            failed = until(lambda: rpc(host, wire.Request(inspect=wire.Empty())).instance,
                lambda state: state.state == 'failed')
            assert 'interrupted' in failed.error and not failed.process_id, failed
            stopped = until(lambda: account_snapshot(host),
                lambda state: state.orders[0].status == 'cancelled')
            assert stopped.orders[0].id == order_id and len(stopped.orders) == 1, stopped
            with sqlite3.connect(root / 'runs/live-window.sqlite') as db:
                saved = wire.Instance.FromString(db.execute('SELECT value FROM configuration').fetchone()[0])
                assert saved == config
                rows = db.execute('SELECT input,decision FROM steps ORDER BY sequence').fetchall()
            assert len(rows) == 2
            namespace = {}; exec(CODE, namespace)
            strategy, context = namespace['TwoBars'](), Context(config.contracts)
            for index, (raw, result) in enumerate(rows, 1):
                batch, decision = wire.Bars.FromString(raw), wire.Decision.FromString(result)
                assert batch.sequence == decision.sequence == index
                bars = []
                for row in batch.bars:
                    assert row.end_ms % 60000 == 0 and row.end_ms - 60000 > started_ms
                    assert row.HasField('open_interest_units') and row.volume > 0
                    bars.append(Bar(row.contract, row.contract,
                        datetime.datetime.fromtimestamp(row.end_ms / 1000,
                            datetime.timezone(datetime.timedelta(hours=8))),
                        row.open_units / 1e8, row.high_units / 1e8, row.low_units / 1e8,
                        row.close_units / 1e8, row.volume, row.open_interest_units / 1e8))
                wanted = step(strategy, context, bars)
                assert context.ready == decision.ready
                assert wanted == {target.contract: target.lots for target in decision.targets}
            assert rpc(host, wire.Request(start=config), True).code == 'conflict'
            (root / 'ctp-flow/disconnect').unlink()
            config.id = 'fresh-window'
            fresh = rpc(host, wire.Request(start=config)).instance
            assert fresh.state == 'warming' and not fresh.completed_steps and not fresh.HasField('last_decision')
            rpc(host, wire.Request(stop=wire.Stop(instance_id=config.id)))
            assert len(account_snapshot(host).orders) == 1
            assert account_snapshot(host).HasField('authorization')
            print('Direct Trading feed, complete live minutes, readiness, target execution, disconnect cancellation, exact input replay and empty restart passed')
        finally:
            terminate(process)


if __name__ == '__main__':
    main()
