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
from strategy_process import source, build, rpc, until, terminate, initialize_account, account_snapshot, account_orders, account_command
from trading_installation import install, Programs
from asterion import common_pb2 as common, strategy_pb2 as wire, trading_pb2 as trading
from asterion_strategy import Bar, Context, step

NEXT_YEAR = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).year + 1
DELIVERY_MONTH = f"{NEXT_YEAR}-10"
SYMBOL = f"rb{NEXT_YEAR % 100:02d}10"
CANONICAL = f"SHFE/rb/{DELIVERY_MONTH}"

CODE = f'''from asterion_strategy import Strategy
class TwoBars(Strategy):
    params = {{"window": 2, "lots": 3}}
    def ready(self, ctx):
        return all(len(ctx.history(s, "close", self.params["window"])) == self.params["window"]
                   for s in ctx.symbols)
    def on_bar(self, ctx, bar):
        # As a backtest names the contract; another name ends the instance.
        assert bar.symbol == "{CANONICAL}", bar.symbol
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
        shutil.copy2(source / 'apps/services/trading/python/strategy_runner.py', code)
        shutil.copytree(source / 'apps/services/common/python/asterion_strategy',
                        code / 'asterion_strategy', ignore=shutil.ignore_patterns('__pycache__'))
        for name in ('strategy_pb2.py', 'common_pb2.py'):
            shutil.copy2(build / 'generated-python/asterion' / name, code / 'asterion')
        (host.root / 'runtime').mkdir()
        (host.root / 'runtime/python').symlink_to(Path(sys.executable).resolve().parents[1])
        host.account('account', user='strategy')
        process = host.start('account')
        root = host.root / 'data/account/strategies'
        try:
            initialize_account(host, symbol=SYMBOL, delivery_month=DELIVERY_MONTH)
            account_command(host, trading.Command(request_id='authorize.stream',
                authorize=trading.Authorize(user_id='strategy')))
            manifest = wire.Manifest(name='two-bars', version='1', entry='rule:TwoBars',
                runtime='python3.12-numpy2.3.5-scipy1.17.1', files=['rule.py'])
            rpc(host, 'publish_strategy', wire.Package(manifest=manifest,
                files=[wire.File(path='rule.py', content=CODE.encode())]))
            config = wire.Instance(id='live-window', package_name='two-bars', package_version='1',
                instruments=[dict(exchange='SHFE', symbol=SYMBOL)], period=common.PERIOD_MINUTES_1)
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
            assert not account_snapshot(host).order_count
            ready = until(observe, lambda state: state.state == trading.INSTANCE_STATE_READY, seconds=70)
            assert ready.completed_steps == 2 and ready.last_decision.targets[0].lots == 3, ready
            sent = until(lambda: account_orders(host), lambda orders: len(orders) == 1)
            assert sent[0].lots.text == '3', sent
            order_id = sent[0].id
            # The three-lot order rests past the batch's 10 s: the batch is dropped and its
            # order cancelled once, and the instance keeps running.
            dropped = until(lambda: account_orders(host),
                lambda orders: orders[0].state == trading.ORDER_STATE_CANCELLED, seconds=20)
            assert dropped[0].id == order_id, dropped
            assert observe().state == trading.INSTANCE_STATE_READY
            # A provider disconnect does not end the instance; the interrupted minute is skipped
            # and delivery resumes on the next complete minute after CTP reconnects.
            (root / 'ctp-flow/disconnect').touch()
            time.sleep(2)
            (root / 'ctp-flow/disconnect').unlink()
            until(observe, lambda state: state.completed_steps >= 3, seconds=150)
            live_run = rpc(host, 'get_strategy_run', {})
            assert live_run.configuration.id == config.id and len(live_run.steps) >= 3
            recorded_bars = rpc(host, 'get_recorded_bars', dict(
                period=common.PERIOD_MINUTES_1,
                instrument=dict(exchange='SHFE', symbol=SYMBOL)))
            assert len(recorded_bars.bars) >= 3
            rpc(host, 'stop_strategy', dict(id=config.id))
            with sqlite3.connect(root / 'runs/live-window.sqlite') as db:
                saved = wire.Instance.FromString(db.execute('SELECT value FROM configuration').fetchone()[0])
                assert saved.HasField('warmup_steps') and saved.warmup_steps == 0
                saved.ClearField('warmup_steps')
                assert saved == config
                rows = db.execute('SELECT input,decision FROM steps ORDER BY sequence').fetchall()
                events = [row[0] for row in db.execute('SELECT event FROM events ORDER BY rowid')]
            assert [e for e in events if e.startswith('market')] == ['market disconnected', 'market reconnected'], events
            assert events[0].startswith(f'sent SHFE/{SYMBOL} buy open 3'), events
            assert f'SHFE/{SYMBOL} abandoned: strategy target expired before completion' in events, events
            assert len(rows) >= 3
            step2_end = wire.Bars.FromString(rows[1][0]).bars[0].end_ms
            step3_end = wire.Bars.FromString(rows[2][0]).bars[0].end_ms
            assert step3_end >= step2_end + 120000, (step2_end, step3_end)
            namespace = {}; exec(CODE, namespace)
            symbol = CANONICAL
            strategy, context = namespace['TwoBars'](), Context([symbol])
            for index, (raw, result) in enumerate(rows, 1):
                batch, decision = wire.Bars.FromString(raw), wire.Decision.FromString(result)
                assert batch.sequence == decision.sequence == index
                bars = []
                for row in batch.bars:
                    assert row.end_ms % 60000 == 0 and row.end_ms - 60000 > started_ms
                    assert row.HasField('open_interest') and row.volume > 0
                    assert (row.instrument.exchange, row.instrument.symbol) == ('SHFE', SYMBOL)
                    bars.append(Bar(symbol, symbol,
                        datetime.datetime.fromtimestamp(row.end_ms / 1000,
                            datetime.timezone(datetime.timedelta(hours=8))),
                        float(row.open.text), float(row.high.text), float(row.low.text),
                        float(row.close.text), row.volume, float(row.open_interest.text)))
                wanted = step(strategy, context, bars)
                assert context.ready == decision.ready
                assert wanted == {symbol: t.lots for t in decision.targets}
                assert all(t.instrument.symbol == SYMBOL for t in decision.targets)
            assert rpc(host, 'start_strategy', config, True).code == 'conflict'
            # A warm restart replays the ended instance's record before live bars.
            resumed_config = wire.Instance()
            resumed_config.CopyFrom(config)
            resumed_config.id, resumed_config.resume_from = 'resumed-window', config.id
            resumed = rpc(host, 'start_strategy', resumed_config).instance
            assert resumed.state == trading.INSTANCE_STATE_READY and resumed.completed_steps == len(rows), resumed
            assert rpc(host, 'get_strategy_run', dict(id=resumed_config.id)).configuration.warmup_steps == len(rows)
            assert resumed.last_decision == wire.Decision.FromString(rows[-1][1]), resumed
            paused = rpc(host, 'pause_strategy', dict(id=resumed_config.id, paused=True)).instance
            assert paused.paused and paused.events[-1].text == 'paused', paused
            flattened = rpc(host, 'flatten_strategy', dict(id=resumed_config.id)).instance
            assert flattened.paused and flattened.events[-1].text == 'flatten requested', flattened
            rpc(host, 'stop_strategy', dict(id=resumed_config.id))
            local_config = wire.Instance()
            local_config.CopyFrom(config)
            local_config.id, local_config.local_bars = 'local-window', True
            local_config.params['lots'].integer = 4
            local_resumed = rpc(host, 'start_strategy', local_config).instance
            assert local_resumed.state == trading.INSTANCE_STATE_READY and local_resumed.completed_steps == len(rows)
            assert local_resumed.last_decision.targets[0].lots == 4
            rpc(host, 'stop_strategy', dict(id=local_config.id))
            # Mid-day warmup overlapping bars.sqlite deduplicates by end_ms and accepts missing open_interest.
            import hashlib
            overlap_warmup = wire.Warmup(dataset_version='v-20261116')
            digest = hashlib.sha256()
            first_recorded = wire.Bars.FromString(rows[0][0]).bars[0]
            earlier_end = first_recorded.end_ms - 60000
            for seq, (day, end_ms, close, oi) in enumerate([
                    (first_recorded.trading_day, earlier_end, '3500', ''),
                    (first_recorded.trading_day, first_recorded.end_ms, first_recorded.close.text, '1000')], 1):
                batch = overlap_warmup.bars.add(sequence=seq)
                bar = batch.bars.add(instrument=dict(exchange='SHFE', symbol=SYMBOL),
                    trading_day=day, end_ms=end_ms, open={'text': close}, high={'text': close},
                    low={'text': close}, close={'text': close}, volume=50)
                if oi:
                    bar.open_interest.text = oi
                digest.update(b'batch\n')
                digest.update(f'{day}|{end_ms}|SHFE/{SYMBOL}|{close}|{close}|{close}|{close}|50|{oi}\n'.encode())
            overlap_warmup.sha256 = digest.hexdigest()
            overlap_config = wire.Instance()
            overlap_config.CopyFrom(local_config)
            overlap_config.id = 'overlap-local'
            overlap_config.warmup.CopyFrom(overlap_warmup)
            overlapped = rpc(host, 'start_strategy', overlap_config).instance
            assert overlapped.state == trading.INSTANCE_STATE_READY and overlapped.completed_steps == len(rows) + 1, overlapped
            assert rpc(host, 'get_strategy_run', dict(id=overlap_config.id)).configuration.warmup_steps == len(rows) + 1
            rpc(host, 'stop_strategy', dict(id=overlap_config.id))
            other = wire.Instance()
            other.CopyFrom(resumed_config)
            other.id, other.period = 'other-window', common.PERIOD_MINUTES_5
            assert rpc(host, 'start_strategy', other, True).code == 'invalid_request'
            orders = account_snapshot(host).order_count
            config.id = 'fresh-window'
            fresh = rpc(host, 'start_strategy', config).instance
            assert fresh.state == trading.INSTANCE_STATE_WARMING and not fresh.completed_steps and not fresh.HasField('last_decision')
            rpc(host, 'stop_strategy', dict(id=config.id))
            assert account_snapshot(host).order_count == orders
            assert account_snapshot(host).HasField('authorized_ms')
            print('Direct Trading feed, complete live minutes, readiness, batch expiry, surviving a '
                  'disconnect, exact input replay, warm restart and empty restart passed')
        finally:
            terminate(process)


if __name__ == '__main__':
    main()
