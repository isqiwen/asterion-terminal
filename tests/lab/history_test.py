"""Research clients against the real C++ History, using test-only Parquet data."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
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
from asterion import common_pb2 as common, history_pb2 as market
from asterion.common_pb2 import Period
from asterion.history_pb2 import Request as MarketRequest, Response as MarketResponse
from asterion_lab.backtest import COSTS
from asterion_lab.history import Client, Dataset, History, HistoryError
from asterion_lab.service import Service
from asterion_lab.runs import Runs
from history_installation import history_host
import history_tls
from warehouse_fixture import warehouse, DAYS, price_tick
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
        cls.running = history_host(build, cls.host)
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
        history_tls.install_client(self.tls, self.node)
        self.settings = self.node / 'terminal/data/market.json'
        self.settings.write_text(json.dumps(dict(host='127.0.0.1', port=self.port,
            fingerprint=history_tls.fingerprint(self.tls), instruments=[])))
        self.history = History(self.settings.parent)
        self.dataset = Dataset(self.lab, history=self.history)
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

    def test_a_days_price_ticks_reach_research_over_tls_and_stay_fixed_for_two_clients(self):
        from datetime import date
        snapshots = self.data / 'catalog'
        snapshots.mkdir(parents=True, exist_ok=True)
        def snapshot(day, tick):
            catalog = market.Catalog()
            item = catalog.contracts.add()
            item.contract = 'SHFE/rb/2027-01'
            item.price_tick.text = tick
            (snapshots / (day + '.pb')).write_bytes(catalog.SerializeToString())
        def rpc(operation, role='reader', **values):
            request = MarketRequest(id=1)
            call = getattr(request, operation)
            call.SetInParent()
            call.CopyFrom(type(call)(**values))
            payload = request.SerializeToString()
            with history_tls.connect(self.tls, self.port, role) as stream:
                stream.sendall(len(payload).to_bytes(4, 'big') + payload)
                def read(n):
                    out = bytearray()
                    while len(out) < n:
                        chunk = stream.recv(n-len(out))
                        self.assertTrue(chunk)
                        out.extend(chunk)
                    return bytes(out)
                return MarketResponse.FromString(read(int.from_bytes(read(4), 'big')))
        left = Dataset(self.lab, history=self.history)
        right = Dataset(self.node / 'second-lab', history=self.history)
        def composed_with(day):
            # History composes research from the snapshot soon after taking it in.
            deadline = time.monotonic() + 15
            while True:
                identity = left.current().identity
                if identity and any(key.endswith(day) for key in left.version(identity).keys('price_ticks')):
                    return identity
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
        # Reading needs no permission to manage; starting a task does.
        values = dict(id='denied', table=market.TABLE_PRICE_TICKS,
                      first_day='2026-10-28', last_day='2026-10-28')
        self.assertEqual(rpc('start_acquisition', **values).error.code,
                         common.ERROR_CODE_PERMISSION_DENIED)
        snapshot('20261028', '1')
        identity = composed_with('2026-10-28')
        sql = ("select effective_from, price_tick from price_ticks "
               "where effective_from = date '2026-10-28'")
        expected = [(date(2026, 10, 28), Decimal('1'))]
        for dataset in (left, right):
            self.assertEqual(dataset.query(dataset.version(identity), sql, ('price_ticks',)), expected)
        snapshot('20261029', '2')
        updated = composed_with('2026-10-29')
        self.assertNotEqual(updated, identity)
        self.assertEqual(right.query(right.version(identity), sql, ('price_ticks',)), expected)
        # A new cache also downloads the old version after a new one becomes current.
        cold = Dataset(self.node / 'cold-lab', history=self.history)
        self.assertEqual(cold.query(cold.version(identity), sql, ('price_ticks',)), expected)

    def test_research_never_falls_back_to_a_local_warehouse(self):
        warehouse(self.lab)
        # A local fixture exists but belongs to no remote version selection.
        self.assertNotEqual(Warehouse(self.lab).current().identity, self.original.identity)
        self.assertEqual(self.dataset.current().identity, self.original.identity)

    def test_price_tick_history_downloads_with_its_exact_value_and_fixed_version(self):
        symbol = 'SHFE/rb/2026-05'
        writer = Warehouse(self.data).writer()
        row = price_tick(symbol, '0.00000001')
        writer.put_rows('price_ticks/SHFE', 'price_ticks', [row])
        pinned = writer.commit().identity
        writer.put_rows('price_ticks/SHFE', 'price_ticks', [price_tick(symbol, '0.2')])
        writer.commit()
        sql = 'select effective_from, effective_until, price_tick, source_url from price_ticks'
        version = self.dataset.version(pinned)
        actual, = self.dataset.query(version, sql, ('price_ticks',))
        self.assertEqual((actual[0].isoformat(), actual[1].isoformat(), actual[2], actual[3]),
                         (row['effective_from'], row['effective_until'], Decimal('0.00000001'), row['source_url']))
        current, = self.dataset.query(self.dataset.current(), sql, ('price_ticks',))
        self.assertEqual(current[2], Decimal('0.2'))

    def test_two_clients_keep_their_fixed_version_and_refetch_after_selection_changes(self):
        other = Dataset(self.node / 'other-lab', history=self.history)
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
        with self.assertRaises(HistoryError):
            self.dataset.current()
        self.assertEqual(self.count(other, second), 60)
        self.assertFalse((self.lab / 'warehouse').exists())
        self.assertFalse((self.dataset.root / 'current').exists())

    def test_version_pages_ignore_a_new_current_published_between_pages(self):
        call, versions = Client.call, []
        def pages(client, operation, **values):
            reply = call(client, operation, **values)
            if operation == 'get_dataset_version':
                versions.append(values['dataset']['version'])
                # History's pages are larger than this dataset; split them to one partition.
                if len(reply.partitions) > 1:
                    del reply.partitions[1:]
                    reply.next_key = reply.partitions[0].key
            if operation == 'get_dataset_version' and len(versions) == 1:
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
        service = Service('lab', self.lab, Log(), self.history)
        run = self.base().start_backtest
        run.period = Period.PERIOD_MINUTES_1
        origin = self.history.selected()
        try:
            with patch.object(self.history, 'selected', side_effect=[origin, AssertionError('selection read twice')]):
                pinned = service.pin_backtest(run)
            self.assertEqual(pinned.pinned.dataset_version, version)
            self.settings.unlink()
            self.assertEqual(service.pin_backtest(run, version), pinned)
        finally:
            service.reads.shutdown(wait=True)

    def test_a_contract_s_minutes_download_only_its_days_and_coverage_downloads_nothing(self):
        writer = Warehouse(self.data).writer()
        for month in ('2026-05', '2026-06'):
            for day in ('2026-01-05', '2026-01-06', '2026-01-07'):
                writer.put_rows(f'minutes/SHFE/{day}/rb/{month}', 'minutes', [], days=[day])
        writer.commit()
        dataset = Dataset(self.lab, history=self.history)
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
            reply = call(client, operation, **values)
            if operation == 'read_dataset_file':
                # History's chunks are larger than this file; split them to 256 bytes.
                offsets.append(values['offset'])
                reply.data = reply.data[:256]
            return reply
        with patch.object(Client, 'call', small), ThreadPoolExecutor(2) as pool:
            jobs = [pool.submit(self.dataset.read, version, key) for _ in range(2)]
            self.assertEqual([job.result().height for job in jobs], [40, 40])
        self.assertGreater(len(offsets), 1)
        self.assertEqual(offsets.count(0), 1)
        with self.assertRaises(HistoryError):
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
                if operation == 'read_dataset_file':
                    if failure == 'disconnect':
                        raise HistoryError('unavailable', 'connection lost')
                    item.data = bytes([item.data[0] ^ 1]) + item.data[1:]
                return item
            with patch.object(Client, 'call', broken), self.assertRaises(HistoryError):
                self.dataset.read(version, key)
            self.assertFalse(path.exists())
            self.assertEqual(list(path.parent.glob('.download-*')), [])
        self.assertEqual(self.dataset.read(version, key).height, 40)
        wrong = self.history.selected()
        wrong['fingerprint'] = '0' * 64
        with Client(wrong) as client, self.assertRaisesRegex(HistoryError, 'fingerprint'):
            client.call('get_dataset_version', dataset=dict())

    def base(self):
        request = Request(id=1)
        run = request.start_backtest
        run.repository, run.strategy = str(self.repository), 'ma_cross.MaCross'
        run.symbols.append('SHFE/rb/2026-05')
        run.params['fast'].integer, run.params['slow'].integer = 2, 4
        run.first_day, run.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
        run.period = Period.PERIOD_DAY
        run.deposit.text = '1000000'
        run.limits.max_order_lots.text = run.limits.max_gross_lots.text = '10'
        run.limits.max_working_orders = 10
        run.periods_per_year, run.zero_risk_free_rate = 252, True
        costs = run.costs.add(product='SHFE/rb').costs
        for name, value in zip(COSTS, ['0', '3', '3', '3', '0.1', '0', '0', '0']):
            getattr(costs, name).text = value
        return request

    def test_real_children_sweeps_rolling_factors_and_charts_read_the_admitted_version(self):
        async def scenario():
            service = Service('lab', self.lab, Log(), self.history)
            try:
                request = self.base()
                request.start_backtest.id = 'single'
                # Pin at admission, then publish before the child downloads its first file.
                launch = service.launch
                async def changed(*args, **kwargs):
                    self.publish_incomplete()
                    return await launch(*args, **kwargs)
                service.launch = changed
                answer = await service.reply(request)
                self.assertEqual(answer.WhichOneof('result'), 'start_backtest', answer)
                await self.finished(service)
                record = Runs(self.lab).record('single')
                self.assertEqual(record['data']['dataset_version'], self.original.identity)
                self.assertEqual(Runs(self.lab).list()[0]['state'], 'succeeded')
                self.research.write_text(self.original.identity)
                sweep = Request(id=1)
                sweep.start_sweep.id, sweep.start_sweep.processes = 'scan', 1
                sweep.start_sweep.base.CopyFrom(self.base().start_backtest)
                sweep.start_sweep.values.add(name='fast').values.add(integer=2)
                sweep.start_sweep.values[0].values.add(integer=3)
                sweep.start_sweep.walk_forward.training_days = 20
                sweep.start_sweep.walk_forward.validation_days = 5
                answer = await service.reply(sweep)
                self.assertEqual(answer.WhichOneof('result'), 'start_sweep', answer)
                await self.finished(service)
                rows = Runs(self.lab).list()
                self.assertEqual(len(rows), 4)
                self.assertEqual({row['state'] for row in rows}, {'succeeded'}, rows)
                self.assertEqual({row['dataset_version'] for row in rows}, {self.original.identity})
                # A factor reads the version current when it is admitted.
                self.research.write_text(self.original.identity)
                factor = Request(id=1)
                run = factor.evaluate_factor
                run.repository, run.factor = str(self.repository), 'price.momentum'
                run.params['lookback'].integer = 2
                run.symbols.extend(['SHFE/rb/2026-05', 'SHFE/rb/2026-10', 'SHFE/rb'])
                run.first_day, run.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
                run.horizons.extend([1, 3])
                run.groups = 3
                answer = await service.reply(factor)
                self.assertEqual(answer.WhichOneof('result'), 'evaluate_factor', answer)
                self.assertEqual(answer.evaluate_factor.pinned.dataset_version, self.original.identity)
                query = Request(id=1, get_chart_bars=dict(
                    dataset_version=self.original.identity, contract='SHFE/rb/2026-05',
                    period=Period.PERIOD_DAY))
                answer = await service.reply(query)
                self.assertEqual(answer.WhichOneof('result'), 'get_chart_bars', answer)
                self.assertEqual(len(answer.get_chart_bars.bars), 30)
                self.publish_incomplete()
                query.get_chart_bars.dataset_version = ''
                self.assertLess(len((await service.reply(query)).get_chart_bars.bars), 30)
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
