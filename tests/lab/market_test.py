"""Research clients against the real C++ Market, using test-only Parquet data."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
import tempfile
import time
import unittest
from unittest.mock import patch

source, build = Path(sys.argv.pop(1)), Path(sys.argv.pop(1))
sys.path[:0] = [str(source / 'apps/services/lab'), str(build / 'generated-python'),
               str(build / 'engine-python'), str(source / 'tests/support'), str(Path(__file__).parent)]
from asterion.lab_pb2 import Request
from asterion.market_pb2 import Request as MarketRequest, Response as MarketResponse
from asterion_lab.market import Client, Dataset, Market, MarketError
from asterion_lab.service import Service
from asterion_lab.runs import Runs
from market_installation import market_host
import market_tls
from warehouse_fixture import warehouse, DAYS
from warehouse_writer import Warehouse


class Log:
    def write(self, *args, **kwargs):
        pass


class RemoteResearch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='asterion-lab-market-')
        cls.root = Path(cls.temporary.name)
        cls.host = cls.root / 'market'
        cls.running = market_host(build, cls.host)
        cls.port = cls.running.__enter__()
        cls.tls = cls.host / 'config/tls'
        cls.data = cls.host / 'data'
        warehouse(cls.data)
        cls.original = Warehouse(cls.data).current()
        cls.repository = cls.root / 'strategies'
        shutil.copytree(source / 'tests/lab/strategies', cls.repository)
        for args in [('init', '-q'), ('add', '.'), ('commit', '-qm', 'fixture')]:
            subprocess.run(['git', '-C', str(cls.repository), '-c', 'user.name=test',
                            '-c', 'user.email=test@invalid', *args], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.running.__exit__(None, None, None)
        cls.temporary.cleanup()

    def setUp(self):
        self.client = tempfile.TemporaryDirectory(dir=self.root)
        self.node = Path(self.client.name)
        self.lab = self.node / 'lab'
        self.lab.mkdir()
        market_tls.install_client(self.tls, self.node)
        self.settings = self.node / 'terminal/data/market.json'
        self.settings.write_text(json.dumps(dict(host='127.0.0.1', port=self.port,
            fingerprint=market_tls.fingerprint(self.tls), instruments=[])))
        self.market = Market(self.settings.parent)
        self.dataset = Dataset(self.lab, market=self.market)
        self.research = self.data / 'sources/research/warehouse/current'
        self.research.write_text(self.original.identity)

    def tearDown(self):
        self.client.cleanup()

    def publish_incomplete(self):
        writer = Warehouse(self.data).writer()
        writer.put_rows('daily/SHFE/2026-01', 'daily', [], days=[])
        return writer.commit().identity

    def count(self, dataset, version):
        return dataset.query(version, 'select count(*) from daily', ('daily',))[0][0]

    def test_recorded_minutes_publish_over_tls_and_remain_fixed_for_two_research_clients(self):
        import polars as pl
        from decimal import Decimal
        from datetime import date
        recordings = self.data / 'recordings'
        minutes = recordings / 'minutes/trading_day=20261028'
        segments = recordings / 'segments/trading_day=20261028'
        minutes.mkdir(parents=True)
        segments.mkdir(parents=True)
        base = 1793164800000
        def record(name, start, price):
            # Test-only native archive, including an incomplete minute and a gap.
            schema = dict(venue=pl.String, symbol=pl.String, start_ms=pl.Int64,
                **{key: pl.Decimal(18, 8) for key in ('open', 'high', 'low', 'close')},
                volume=pl.Int64, average_price=pl.Decimal(18, 8),
                open_interest=pl.Decimal(18, 8), partial=pl.Boolean)
            pl.DataFrame([dict(venue='SHFE', symbol='rb2701', start_ms=start,
                open=Decimal(price), high=Decimal(price), low=Decimal(price), close=Decimal(price),
                volume=9007199254740993, average_price=None, open_interest=Decimal('2000'),
                partial=True)], schema=schema).write_parquet(minutes / (name + '.parquet'))
            pl.DataFrame(dict(instance=[name], segment=[1], first_received_ms=[start+5000],
                last_received_ms=[start+55000], ticks=[20], end=['stopped'])).write_parquet(
                    segments / (name + '.parquet'))
        def rpc(operation, role='reader', **values):
            request = MarketRequest(version=1, service_id='market-data', correlation_id=operation)
            getattr(request, operation).SetInParent()
            for key, value in values.items():
                setattr(getattr(request, operation), key, value)
            payload = request.SerializeToString()
            with market_tls.connect(self.tls, self.port, role) as stream:
                stream.sendall(len(payload).to_bytes(4, 'big') + payload)
                def read(n):
                    out = bytearray()
                    while len(out) < n:
                        chunk = stream.recv(n-len(out))
                        self.assertTrue(chunk)
                        out.extend(chunk)
                    return bytes(out)
                return MarketResponse.FromString(read(int.from_bytes(read(4), 'big')))
        def publish(identity, refresh=False):
            values = dict(id=identity, provider='recorder', table='minutes',
                contract='SHFE/rb/2027-01', begin_day='2026-10-28', end_day='2026-10-28',
                refresh=refresh)
            self.assertEqual(rpc('acquisition_start', **values).error.code, 'permission_denied')
            response = rpc('acquisition_start', 'manager', **values)
            self.assertFalse(response.HasField('error'), response)
            # The submitting connection has already closed. Another client watches completion.
            deadline = time.monotonic() + 10
            while True:
                task = rpc('acquisition_task', id=identity).acquisition_task
                if task.state not in ('running', 'queued'):
                    self.assertEqual(task.state, 'succeeded', task)
                    return task.dataset_version
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
        record('first', base, '3100.123456')
        publish('recorded-first')
        # Market composes research from the publication.
        left = Dataset(self.lab, market=self.market)
        right = Dataset(self.node / 'second-lab', market=self.market)
        identity = left.current().identity
        sql = 'select day, start_ms, close, volume, average_price, partial from minutes order by start_ms'
        expected = [(date(2026, 10, 28), base, Decimal('3100.123456'),
                     Decimal('9007199254740993'), None, True)]
        for dataset in (left, right):
            version = dataset.version(identity)
            self.assertEqual(dataset.query(version, sql, ('minutes',)), expected)
            self.assertEqual(dataset.query(version, 'select instance, "end" from observations',
                                            ('observations',)), [('first', 'stopped')])
        record('later', base + 300000, '3101')
        publish('recorded-skip')
        self.assertEqual(left.current().identity, identity)
        publish('recorded-refresh', True)
        updated = left.current().identity
        self.assertNotEqual(updated, identity)
        self.assertEqual(len(left.query(left.version(updated), sql, ('minutes',))), 2)
        self.assertEqual(right.query(right.version(identity), sql, ('minutes',)), expected)
        # A new cache also downloads the old publication after a new one becomes current.
        cold = Dataset(self.node / 'cold-lab', market=self.market)
        self.assertEqual(cold.query(cold.version(identity), sql, ('minutes',)), expected)

    def test_research_never_falls_back_to_a_local_warehouse(self):
        warehouse(self.lab)
        # A local fixture exists but belongs to no remote version selection.
        self.assertNotEqual(Warehouse(self.lab).current().identity, self.original.identity)
        self.assertEqual(self.dataset.current().identity, self.original.identity)

    def test_two_clients_keep_their_fixed_version_and_refetch_after_selection_changes(self):
        other = Dataset(self.node / 'other-lab', market=self.market)
        with ThreadPoolExecutor(2) as pool:
            first, second = [item.result() for item in
                             [pool.submit(self.dataset.current), pool.submit(other.current)]]
        self.assertEqual(first.identity, second.identity)
        new = self.publish_incomplete()
        self.assertEqual(other.current().identity, new)
        self.assertEqual(self.count(self.dataset, first), 60)
        self.assertEqual(self.count(other, second), 60)
        path = self.dataset.root / first.manifest['partitions']['daily/SHFE/2026-01']['file']
        path.unlink()
        changed = json.loads(self.settings.read_text())
        changed['port'] = 1
        self.settings.write_text(json.dumps(changed))
        # No retargeting: a child with no current selection uses the stored origin.
        child = Dataset(self.lab)
        self.assertEqual(self.count(child, child.version(first.identity)), 60)
        with self.assertRaises(MarketError):
            self.dataset.current()
        self.assertEqual(self.count(other, second), 60)
        self.assertFalse((self.lab / 'warehouse').exists())
        self.assertFalse((self.dataset.root / 'current').exists())

    def test_version_pages_ignore_a_new_current_published_between_pages(self):
        call, versions = Client.call, []
        def pages(client, operation, **values):
            if operation == 'dataset':
                values['limit'] = 1
                versions.append(values['dataset_version'])
            reply = call(client, operation, **values)
            if operation == 'dataset' and len(versions) == 1:
                self.publish_incomplete()
            return reply
        with patch.object(Client, 'call', pages):
            version = self.dataset.current()
        self.assertEqual(version.identity, self.original.identity)
        self.assertEqual(set(version.manifest['partitions']), set(self.original.manifest['partitions']))
        self.assertEqual(versions, [''] + [self.original.identity] * (len(versions) - 1))
        self.assertGreater(len(versions), 1)
        self.assertEqual(self.count(self.dataset, version), 60)

    def test_daily_and_minute_admission_uses_one_market_selection(self):
        writer = Warehouse(self.data).writer()
        writer.put_rows('minutes/SHFE/2026-01-05/rb/2026-05', 'minutes', [], days=['2026-01-05'])
        version = writer.commit().identity
        service = Service('lab', self.lab, Log(), self.market)
        run = self.base().backtest
        run.minutes = 1
        origin = self.market.selected()
        try:
            with patch.object(self.market, 'selected', side_effect=[origin, AssertionError('selection read twice')]):
                pinned = service.pin_backtest(run)
            self.assertEqual(pinned.dataset_version, version)
            self.settings.unlink()
            self.assertEqual(service.pin_backtest(pinned), pinned)
        finally:
            service.reads.shutdown(wait=True)

    def test_a_contract_s_minutes_download_only_its_days_and_coverage_downloads_nothing(self):
        writer = Warehouse(self.data).writer()
        for month in ('2026-05', '2026-06'):
            for day in ('2026-01-05', '2026-01-06', '2026-01-07'):
                writer.put_rows(f'minutes/SHFE/{day}/rb/{month}', 'minutes', [], days=[day])
        writer.commit()
        dataset = Dataset(self.lab, market=self.market)
        version = dataset.current()
        fetched, ensure = [], Dataset.ensure
        def record(cache, version, key):
            fetched.append(key)
            return ensure(cache, version, key)
        with patch.object(Dataset, 'ensure', record):
            self.assertEqual(version.minute_coverage('SHFE/rb/2026-05'),
                             {'SHFE/rb/2026-05': ('2026-01-05', '2026-01-07', 3)})
            self.assertEqual(fetched, [])
            dataset.query(version, 'select count(*) from minutes', ('minutes',),
                          contract='SHFE/rb/2026-05', begin='2026-01-06', end='2026-01-07')
        self.assertEqual(fetched, ['minutes/SHFE/2026-01-06/rb/2026-05',
                                   'minutes/SHFE/2026-01-07/rb/2026-05'])

    def test_concurrent_chunk_downloads_publish_once_and_missing_versions_never_use_latest(self):
        version = self.dataset.current()
        key = 'daily/SHFE/2026-01'
        offsets, call = [], Client.call
        def small(client, operation, **values):
            if operation == 'dataset_file':
                values['limit'] = 256
                offsets.append(values['offset'])
            return call(client, operation, **values)
        with patch.object(Client, 'call', small), ThreadPoolExecutor(2) as pool:
            jobs = [pool.submit(self.dataset.read, version, key) for _ in range(2)]
            self.assertEqual([job.result().height for job in jobs], [40, 40])
        self.assertGreater(len(offsets), 1)
        self.assertEqual(offsets.count(0), 1)
        with self.assertRaises(MarketError):
            self.dataset.version('a' * 64)
        self.assertFalse((self.dataset.root / 'versions' / ('a' * 64 + '.json')).exists())
        shutil.rmtree(self.dataset.root)
        self.assertEqual(self.dataset.version(version.identity).identity, version.identity)

    def test_bad_or_interrupted_downloads_are_not_published_and_wrong_pin_fails(self):
        version = self.dataset.current()
        key = 'daily/SHFE/2026-01'
        path = self.dataset.root / version.manifest['partitions'][key]['file']
        call = Client.call
        for failure in ('digest', 'disconnect'):
            def broken(client, operation, **values):
                item = call(client, operation, **values)
                if operation == 'dataset_file':
                    if failure == 'disconnect':
                        raise MarketError('unavailable', 'connection lost')
                    item.data = bytes([item.data[0] ^ 1]) + item.data[1:]
                return item
            with patch.object(Client, 'call', broken), self.assertRaises(MarketError):
                self.dataset.read(version, key)
            self.assertFalse(path.exists())
            self.assertEqual(list(path.parent.glob('.download-*')), [])
        self.assertEqual(self.dataset.read(version, key).height, 40)
        wrong = self.market.selected()
        wrong['fingerprint'] = '0' * 64
        with Client(wrong) as client, self.assertRaisesRegex(MarketError, 'fingerprint'):
            client.call('dataset', provider='research')

    def base(self):
        request = Request(version=1, service_id='lab')
        run = request.backtest
        run.repository, run.strategy = str(self.repository), 'ma_cross.MaCross'
        run.symbols.append('SHFE/rb/2026-05')
        run.params['fast'].integer, run.params['slow'].integer = 2, 4
        run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
        run.deposit, run.max_order_lots, run.max_gross_lots = '1000000', '10', '10'
        run.max_working_orders = 10
        run.annual_periods, run.zero_risk_free = 252, True
        run.costs.add(product='SHFE/rb', values=['0', '3', '3', '3', '0.1', '0', '0', '0'])
        return request

    def test_real_children_sweeps_rolling_factors_and_charts_read_the_admitted_version(self):
        async def scenario():
            service = Service('lab', self.lab, Log(), self.market)
            try:
                request = self.base()
                request.backtest.id = 'single'
                # Pin at admission, then publish before the child downloads its first file.
                launch = service.launch
                async def changed(*args, **kwargs):
                    self.publish_incomplete()
                    return await launch(*args, **kwargs)
                service.launch = changed
                answer = await service.reply(request)
                self.assertEqual(answer.WhichOneof('body'), 'accepted', answer)
                await self.finished(service)
                record = Runs(self.lab).record('single')
                self.assertEqual(record['data']['dataset_version'], self.original.identity)
                self.assertEqual(Runs(self.lab).list()[0]['state'], 'succeeded')
                self.research.write_text(self.original.identity)
                sweep = Request(version=1, service_id='lab')
                sweep.sweep.id, sweep.sweep.processes = 'scan', 1
                sweep.sweep.base.CopyFrom(self.base().backtest)
                sweep.sweep.values.add(name='fast').values.add(integer=2)
                sweep.sweep.values[0].values.add(integer=3)
                sweep.sweep.walk_forward.training_days = 20
                sweep.sweep.walk_forward.validation_days = 5
                answer = await service.reply(sweep)
                self.assertEqual(answer.WhichOneof('body'), 'accepted', answer)
                await self.finished(service)
                rows = Runs(self.lab).list()
                self.assertEqual(len(rows), 4)
                self.assertEqual({row['state'] for row in rows}, {'succeeded'}, rows)
                self.assertEqual({row['dataset_version'] for row in rows}, {self.original.identity})
                factor = Request(version=1, service_id='lab')
                run = factor.factor
                run.repository, run.factor = str(self.repository), 'price.momentum'
                run.dataset_version = self.original.identity
                run.params['lookback'].integer = 2
                run.symbols.extend(['SHFE/rb/2026-05', 'SHFE/rb/2026-10', 'SHFE/rb'])
                run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
                run.horizons.extend([1, 3])
                run.groups = 3
                answer = await service.reply(factor)
                self.assertEqual(answer.WhichOneof('body'), 'factor', answer)
                self.assertEqual(answer.factor.dataset_version, self.original.identity)
                query = Request(version=1, service_id='lab', chart_bars=dict(
                    dataset_version=self.original.identity, contract='SHFE/rb/2026-05', period='day'))
                answer = await service.reply(query)
                self.assertEqual(answer.WhichOneof('body'), 'chart_bars', answer)
                self.assertEqual(len(answer.chart_bars.bars), 30)
                query.chart_bars.dataset_version = ''
                self.assertLess(len((await service.reply(query)).chart_bars.bars), 30)
            finally:
                for process in service.backtests.values():
                    process.terminate()
                service.reads.shutdown(wait=True)
        asyncio.run(scenario())

    async def finished(self, service):
        deadline = time.monotonic() + 40
        while service.backtests or service.waiting or service.rolling:
            self.assertLess(time.monotonic(), deadline, 'research children did not finish')
            await asyncio.sleep(.05)


if __name__ == '__main__':
    unittest.main()
