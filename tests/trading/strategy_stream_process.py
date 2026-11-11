"""Actual C++ Trading + test CTP stream + Python child, on the real clock.

Waits for three complete minute bars (at most about four minutes); does not
accelerate timestamps past the production freshness check. Only an isolated
test account is used.
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
from asterion import common_pb2 as common, strategy_pb2 as wire, trading_pb2 as trading
from asterion_strategy import Bar, Context, step

CODE = '''from asterion_strategy import Strategy
class TwoBars(Strategy):
    params = {"window": 2, "lots": 3}
    def ready(self, ctx):
        return all(len(ctx.history(s, "close", self.params["window"])) == self.params["window"]
                   for s in ctx.symbols)
    def on_bar(self, ctx, bar):
        # As a backtest names the contract; another name ends the instance.
        assert bar.symbol == "SHFE/rb/2026-10", bar.symbol
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
        for name in ('strategy_pb2.py', 'common_pb2.py'):
            shutil.copy2(build / 'generated-python/asterion' / name, code / 'asterion')
        (host.root / 'runtime').mkdir()
        (host.root / 'runtime/python').symlink_to(Path(sys.executable).resolve().parents[1])
        host.account('account', user='strategy')
        process = host.start('account')
        root = host.root / 'data/account/strategies'
        try:
            initialize_account(host)
            account_command(host, trading.Command(request_id='authorize.stream',
                authorize=trading.Authorize(user_id='strategy')))
            manifest = wire.Manifest(name='two-bars', version='1', entry='rule:TwoBars',
                runtime='python3.12-numpy2.3.5', files=['rule.py'])
            rpc(host, 'publish_strategy', wire.Package(manifest=manifest,
                files=[wire.File(path='rule.py', content=CODE.encode())]))
            config = wire.Instance(id='live-window', package_name='two-bars', package_version='1',
                instruments=[dict(exchange='SHFE', symbol='rb2610')], period=common.PERIOD_MINUTES_1)
            started_ms = int(time.time() * 1000)
            initial = rpc(host, 'start_strategy', config).instance
            assert initial.state == trading.INSTANCE_STATE_WARMING and not initial.completed_steps, initial
            def observe():
                state = rpc(host, 'get_strategies', {}).instance
                assert state.state in (trading.INSTANCE_STATE_WARMING, trading.INSTANCE_STATE_READY), state
                return state
            first = until(observe, lambda state: state.completed_steps >= 1, seconds=130)
            assert first.completed_steps == 1 and first.state == trading.INSTANCE_STATE_WARMING, first
            assert not first.last_decision.targets and not first.last_decision.ready
            assert not account_snapshot(host).orders
            ready = until(observe, lambda state: state.state == trading.INSTANCE_STATE_READY, seconds=70)
            assert ready.completed_steps == 2 and ready.last_decision.targets[0].lots == 3, ready
            sent = until(lambda: account_snapshot(host), lambda state: len(state.orders) == 1)
            assert sent.orders[0].lots.text == '3', sent
            order_id = sent.orders[0].id
            # The three-lot order rests past the batch's 10 s: the batch is dropped and its
            # order cancelled once, and the instance keeps running.
            dropped = until(lambda: account_snapshot(host),
                lambda state: state.orders[0].state == trading.ORDER_STATE_CANCELLED, seconds=20)
            assert dropped.orders[0].id == order_id, dropped
            assert observe().state == trading.INSTANCE_STATE_READY
            # A provider disconnect does not end the instance; bars continue after CTP reconnects.
            (root / 'ctp-flow/disconnect').touch()
            time.sleep(2)
            (root / 'ctp-flow/disconnect').unlink()
            until(observe, lambda state: state.completed_steps >= 3, seconds=130)
            rpc(host, 'stop_strategy', dict(id=config.id))
            with sqlite3.connect(root / 'runs/live-window.sqlite') as db:
                saved = wire.Instance.FromString(db.execute('SELECT value FROM configuration').fetchone()[0])
                assert saved == config
                rows = db.execute('SELECT input,decision FROM steps ORDER BY sequence').fetchall()
                events = [row[0] for row in db.execute('SELECT event FROM events ORDER BY rowid')]
            assert [e for e in events if e.startswith('market')] == ['market disconnected', 'market reconnected'], events
            assert events[0].startswith('sent SHFE/rb2610 buy open 3'), events
            assert 'batch abandoned: strategy target expired before completion' in events, events
            assert len(rows) >= 3
            namespace = {}; exec(CODE, namespace)
            symbol = 'SHFE/rb/2026-10'
            strategy, context = namespace['TwoBars'](), Context([symbol])
            for index, (raw, result) in enumerate(rows, 1):
                batch, decision = wire.Bars.FromString(raw), wire.Decision.FromString(result)
                assert batch.sequence == decision.sequence == index
                bars = []
                for row in batch.bars:
                    assert row.end_ms % 60000 == 0 and row.end_ms - 60000 > started_ms
                    assert row.HasField('open_interest') and row.volume > 0
                    assert (row.instrument.exchange, row.instrument.symbol) == ('SHFE', 'rb2610')
                    bars.append(Bar(symbol, symbol,
                        datetime.datetime.fromtimestamp(row.end_ms / 1000,
                            datetime.timezone(datetime.timedelta(hours=8))),
                        float(row.open.text), float(row.high.text), float(row.low.text),
                        float(row.close.text), row.volume, float(row.open_interest.text)))
                wanted = step(strategy, context, bars)
                assert context.ready == decision.ready
                assert wanted == {symbol: t.lots for t in decision.targets}
                assert all(t.instrument.symbol == 'rb2610' for t in decision.targets)
            assert rpc(host, 'start_strategy', config, True).code == 'conflict'
            # A warm restart replays the ended instance's record before live bars.
            resumed_config = wire.Instance()
            resumed_config.CopyFrom(config)
            resumed_config.id, resumed_config.resume_from = 'resumed-window', config.id
            resumed = rpc(host, 'start_strategy', resumed_config).instance
            assert resumed.state == trading.INSTANCE_STATE_READY and resumed.completed_steps == len(rows), resumed
            assert resumed.last_decision == wire.Decision.FromString(rows[-1][1]), resumed
            paused = rpc(host, 'pause_strategy', dict(id=resumed_config.id, paused=True)).instance
            assert paused.paused and paused.events[-1].text == 'paused', paused
            flattened = rpc(host, 'flatten_strategy', dict(id=resumed_config.id)).instance
            assert flattened.paused and flattened.events[-1].text == 'flatten requested', flattened
            rpc(host, 'stop_strategy', dict(id=resumed_config.id))
            other = wire.Instance()
            other.CopyFrom(resumed_config)
            other.id, other.period = 'other-window', common.PERIOD_MINUTES_5
            assert rpc(host, 'start_strategy', other, True).code == 'invalid_request'
            orders = len(account_snapshot(host).orders)
            config.id = 'fresh-window'
            fresh = rpc(host, 'start_strategy', config).instance
            assert fresh.state == trading.INSTANCE_STATE_WARMING and not fresh.completed_steps and not fresh.HasField('last_decision')
            rpc(host, 'stop_strategy', dict(id=config.id))
            assert len(account_snapshot(host).orders) == orders
            assert account_snapshot(host).HasField('authorized_ms')
            print('Direct Trading feed, complete live minutes, readiness, batch expiry, surviving a '
                  'disconnect, exact input replay, warm restart and empty restart passed')
        finally:
            terminate(process)


if __name__ == '__main__':
    main()
