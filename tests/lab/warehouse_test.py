"""Research clients against the real C++ Warehouse, using test-only Parquet data."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta
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
from asterion.common_pb2 import Period
from asterion_lab.warehouse_client import CHUNK, Client, Dataset, Endpoint, WarehouseError
from asterion_lab.warehouse import SCHEMAS
from asterion_lab.service import Service
from asterion_lab.runs import Runs
from warehouse_installation import warehouse_host
import warehouse_tls
from warehouse_fixture import warehouse, DAYS, contract_row, daily, price_tick
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
        cls.running = warehouse_host(build, cls.host)
        cls.port = cls.running.__enter__()
        cls.tls = cls.host / 'config/tls'
        cls.data = cls.host / 'data'
        warehouse(cls.data)
        cls.original = Warehouse(cls.data).current()
        cls.repository = cls.root / 'strategies'
        shutil.copytree(source / 'research', cls.repository)
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
        warehouse_tls.install_client(self.tls, self.node)
        self.settings = self.node / 'terminal/data/warehouse.json'
        self.settings.write_text(json.dumps(dict(host='127.0.0.1', port=self.port,
            fingerprint=warehouse_tls.fingerprint(self.tls), instruments=[])))
        self.warehouse = Endpoint(self.settings.parent)
        self.dataset = Dataset(self.lab, history=self.warehouse)
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

    def test_versions_reach_two_research_clients_over_tls_and_stay_fixed(self):
        symbol = 'SHFE/rb/2027-01'
        def publish(tick):
            writer = Warehouse(self.data).writer()
            writer.put_rows('price_ticks/SHFE', 'price_ticks', [price_tick(symbol, tick)])
            return writer.commit().identity
        left = Dataset(self.lab, history=self.warehouse)
        right = Dataset(self.node / 'second-lab', history=self.warehouse)
        identity = publish('1')
        sql = 'select price_tick from price_ticks'
        expected = [(Decimal('1'),)]
        for dataset in (left, right):
            self.assertEqual(dataset.query(dataset.version(identity), sql, ('price_ticks',)), expected)
        publish('2')
        self.assertEqual(left.current().identity, Warehouse(self.data).current().identity)
        self.assertNotEqual(left.current().identity, identity)
        self.assertEqual(right.query(right.version(identity), sql, ('price_ticks',)), expected)
        # A new cache also downloads the old version after a new one becomes current.
        cold = Dataset(self.node / 'cold-lab', history=self.warehouse)
        self.assertEqual(cold.query(cold.version(identity), sql, ('price_ticks',)), expected)

    def test_fundamental_reports_keep_observation_times_and_fixed_date_scopes_over_tls(self):
        for table, value in (('warehouse_receipts', 'volume'),
                             ('member_positions', 'long_position'), ('inventory', 'total')):
            with self.subTest(table=table):
                row = dict.fromkeys(SCHEMAS[table])
                row.update(exchange='SHFE', product='rb', day='2026-01-05',
                           observed_ms=1795248000000, **{value: Decimal('353.123456')})
                writer = Warehouse(self.data).writer()
                writer.put_rows(f'{table}/SHFE/2026-01-05', table, [row], days=['2026-01-05'])
                writer.put_rows(f'{table}/SHFE/2026-01-06', table, [{**row, 'day': '2026-01-06'}],
                                days=['2026-01-06'])
                original = self.dataset.version(writer.commit().identity)
                sql = f'select product, {value}, published_ms, observed_ms from {table}'
                scope = dict(exchange='SHFE', begin='2026-01-05', end='2026-01-05')
                expected = [('rb', Decimal('353.123456'), None, 1795248000000)]
                self.assertEqual(self.dataset.query(original, sql, (table,), **scope), expected)
                frame = self.dataset.read(original, f'{table}/SHFE/2026-01-05')
                self.assertEqual(frame.schema, SCHEMAS[table])
                writer.put_rows(f'{table}/SHFE/2026-01-05', table,
                                [{**row, value: Decimal('400'), 'observed_ms': 1795334400000}],
                                days=['2026-01-05'])
                changed = self.dataset.version(writer.commit().identity)
                self.assertEqual(self.dataset.query(original, sql, (table,), **scope), expected)
                self.assertEqual(self.dataset.query(changed, sql, (table,), **scope),
                                 [('rb', Decimal('400'), None, 1795334400000)])

    def test_research_never_falls_back_to_a_local_warehouse(self):
        warehouse(self.lab)
        # A local fixture exists but belongs to no remote version selection.
        self.assertNotEqual(Warehouse(self.lab).current().identity, self.original.identity)
        self.assertEqual(self.dataset.current().identity, self.original.identity)

    def test_the_research_page_reads_the_current_dataset_summary(self):
        async def scenario():
            service = Service('lab', self.lab, Log(), self.warehouse)
            request = Request(id=1)
            request.get_dataset_summary.SetInParent()
            return await service.reply(request)
        answer = asyncio.run(scenario())
        self.assertEqual(answer.get_dataset_summary.dataset.version, self.original.identity, answer)

    def test_cold_contract_catalog_reads_fixed_coverage_without_downloading_daily_bars(self):
        writer = Warehouse(self.data).writer()
        symbols = ['SHFE/rb/2026-05', 'SHFE/rb/2026-10', 'DCE/m/2026-05']
        writer.put_rows('contracts/DCE', 'contracts', [contract_row(symbols[2])])
        writer.put_rows('daily/DCE/2026-01-05', 'daily',
                        [daily(symbols[2], DAYS[0], 3000, 100)], days=[DAYS[0].isoformat()])
        version = writer.commit()
        self.publish_incomplete()

        async def scenario():
            service = Service('lab', self.lab, Log(), self.warehouse)
            request = Request(id=1, list_stored_contracts=dict(dataset_version=version.identity))
            try:
                answer = await service.reply(request)
                self.assertEqual(answer.WhichOneof('result'), 'list_stored_contracts', answer)
                self.assertEqual(answer.list_stored_contracts.dataset_version, version.identity)
                stored = {item.contract: item for item in answer.list_stored_contracts.contracts}
                self.assertEqual(sorted(stored), sorted(symbols))
                self.assertEqual((stored[symbols[0]].name, stored[symbols[0]].daily.days,
                                  stored[symbols[0]].daily.first_day, stored[symbols[0]].daily.last_day),
                                 (symbols[0], 30, DAYS[0].isoformat(), DAYS[-1].isoformat()))
                self.assertEqual(stored[symbols[2]].daily.days, 1)
                request.list_stored_contracts.contract = symbols[0]
                single = await service.reply(request)
                self.assertEqual(list(single.list_stored_contracts.contracts), [stored[symbols[0]]])
                request.list_stored_contracts.contract = 'SHFE/rb'
                non_contract = await service.reply(request)
                self.assertEqual(non_contract.WhichOneof('result'), 'list_stored_contracts', non_contract)
                self.assertEqual(list(non_contract.list_stored_contracts.contracts), [])
                pinned = self.dataset.version(version.identity)
                self.assertEqual(set(self.dataset.daily_coverage(pinned, exchanges={'DCE'})), {symbols[2]})
                files = {path.name for path in (self.dataset.root / 'files').glob('*.parquet')}
                expected = {f'{part["sha256"]}.parquet' for key, part in version.manifest['partitions'].items()
                            if key.startswith('contracts/')}
                self.assertEqual(files, expected)
                self.count(self.dataset, pinned)
                request.list_stored_contracts.contract = ''
                with patch.object(Client, 'call', side_effect=WarehouseError('unavailable', 'offline')):
                    self.assertEqual(await service.reply(request), answer)
                    request.list_stored_contracts.contract = 'SHFE/rb'
                    self.assertEqual(await service.reply(request), non_contract)
            finally:
                service.reads.shutdown(wait=True)
        asyncio.run(scenario())

    def test_cold_contract_catalog_accepts_coverage_larger_than_two_megabytes(self):
        writer = Warehouse(self.data).writer()
        rows = [contract_row('SHFE/rb/2026-05'), contract_row('SHFE/rb/2026-10')]
        rows.extend(contract_row(f'SHFE/rb/{year:04d}-{month:02d}')
                    for year in range(2027, 5800) for month in range(1, 13))
        writer.put_rows('contracts/SHFE', 'contracts', rows)
        version = writer.commit()
        with Client(self.warehouse.selected()) as client:
            raw = client.call('get_product_coverage', version=version.identity, product='')
        self.assertGreater(raw.ByteSize(), 2 * CHUNK)
        pinned = self.dataset.version(version.identity)
        coverage = self.dataset.daily_coverage(pinned)
        self.assertEqual(sorted(coverage), ['SHFE/rb/2026-05', 'SHFE/rb/2026-10'])

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
        other = Dataset(self.node / 'other-lab', history=self.warehouse)
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
        with self.assertRaises(WarehouseError):
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
                # Warehouse's pages are larger than this dataset; split them to one partition.
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
        # Once the current version is cached locally, current() stops after the first page.
        cached = self.dataset.current()
        cached_calls = []

        def count_pages(client, operation, **values):
            reply = call(client, operation, **values)
            if operation == 'get_dataset_version':
                cached_calls.append(values['dataset']['version'])
                if len(reply.partitions) > 1:
                    del reply.partitions[1:]
                    reply.next_key = reply.partitions[0].key
            return reply

        with patch.object(Client, 'call', count_pages):
            again = self.dataset.current()
        self.assertEqual(again.identity, cached.identity)
        self.assertEqual(cached_calls, [''])

    def test_daily_and_minute_admission_uses_one_market_selection(self):
        writer = Warehouse(self.data).writer()
        writer.put_rows('minutes/SHFE/2026-01-05/rb/2026-05', 'minutes', [], days=['2026-01-05'])
        version = writer.commit().identity
        service = Service('lab', self.lab, Log(), self.warehouse)
        run = self.base().start_backtest
        run.period = Period.PERIOD_MINUTES_1
        origin = self.warehouse.selected()
        try:
            with patch.object(self.warehouse, 'selected', side_effect=[origin, AssertionError('selection read twice')]):
                pinned = service.pin_backtest(run)
            self.assertEqual(pinned.pinned.dataset_version, version)
            self.settings.unlink()
            self.assertEqual(service.pin_backtest(run, version), pinned)
        finally:
            service.reads.shutdown(wait=True)

    def test_daily_research_downloads_only_its_padded_date_window(self):
        from asterion_lab import backtest

        contract = 'SHFE/rb/2026-05'
        writer = Warehouse(self.data).writer()
        for offset in (-93, -92, 92, 93):
            day = DAYS[0] + timedelta(days=offset)
            writer.put_rows(f'daily/SHFE/{day}', 'daily', [daily(contract, day, 3000, 100)],
                            days=[day.isoformat()])
        version = writer.commit()
        for exchange_scan in (False, True):
            dataset = Dataset(self.node / str(exchange_scan), history=self.warehouse)
            research = backtest.Data(dataset, DAYS[0].isoformat(), DAYS[0].isoformat(),
                                     version=version.identity)
            if exchange_scan:
                research.load_exchange('SHFE')
            days = research.product('SHFE', 'rb')[contract].days
            self.assertIn(DAYS[0].isoformat(), days)
            for offset in (-93, -92, 92, 93):
                day = (DAYS[0] + timedelta(days=offset)).isoformat()
                part = research.version.manifest['partitions'][f'daily/SHFE/{day}']
                self.assertEqual(day in days, abs(offset) == 92)
                self.assertEqual((dataset.root / part['file']).exists(), abs(offset) == 92)

    def test_a_contract_s_minutes_download_only_its_days_and_coverage_downloads_nothing(self):
        from datetime import datetime
        import socket
        from asterion_lab import backtest, chart

        writer = Warehouse(self.data).writer()
        for month in ('2026-05', '2026-10'):
            for day in DAYS[:4]:
                start = int(datetime(day.year, day.month, day.day, 9, 1, tzinfo=chart.BEIJING).timestamp()) * 1000
                writer.put_rows(f'minutes/SHFE/{day}/rb/{month}', 'minutes', [dict(
                    exchange='SHFE', product='rb', contract=f'SHFE/rb/{month}', day=day.isoformat(),
                    start_ms=start, partial=False, open=Decimal(3000), high=Decimal(3010),
                    low=Decimal(2990), close=Decimal(3000), volume=Decimal(100),
                    average_price=None, open_interest=Decimal(500))], days=[day.isoformat()])
        writer.commit()
        dataset = Dataset(self.lab, history=self.warehouse)
        version = dataset.current()
        fetched, ensure = [], Dataset.ensure
        def record(cache, version, key):
            fetched.append(key)
            return ensure(cache, version, key)
        connects, real_connect = [], socket.create_connection
        def count_connect(*args, **kwargs):
            connects.append(args[0])
            return real_connect(*args, **kwargs)
        with patch.object(Dataset, 'ensure', record), patch.object(socket, 'create_connection', count_connect):
            self.assertEqual(version.minute_coverage('SHFE/rb/2026-05'),
                             {'SHFE/rb/2026-05': ('2026-01-05', '2026-01-08', 4)})
            self.assertEqual(fetched, [])
            dataset.query(version, 'select count(*) from minutes', ('minutes',),
                          contract='SHFE/rb/2026-05', begin='2026-01-06', end='2026-01-07')
            self.assertEqual(len(connects), 1)
        self.assertEqual(fetched, ['minutes/SHFE/2026-01-06/rb/2026-05',
                                   'minutes/SHFE/2026-01-07/rb/2026-05'])
        # Cold-cache multi-month preparation reuses a single TLS session across all missing partitions.
        cold_lab = self.node / 'cold-prepare'
        cold = Dataset(cold_lab, history=self.warehouse)
        pinned = cold.version(version.identity)
        connects.clear()
        config = backtest.Config(
            repository='', strategy='', params={}, symbols=('SHFE/rb/2026-05', 'SHFE/rb/2026-10'),
            begin_day=DAYS[0].isoformat(), end_day=DAYS[3].isoformat(), deposit=Decimal(1000000),
            costs=None, slippage_ticks=0, max_order_lots=Decimal(10), max_gross_lots=Decimal(10),
            max_contract_lots=Decimal(10), max_working_orders=10, minutes=1,
            dataset_version=pinned.identity)
        with patch.object(socket, 'create_connection', count_connect):
            contracts, _, _ = backtest.prepare(
                backtest.Data(cold, config.begin_day, config.end_day, version=pinned.identity), config)
        self.assertEqual(len(contracts), 2)
        self.assertEqual(len(connects), 1)

    def test_concurrent_chunk_downloads_publish_once_and_missing_versions_never_use_latest(self):
        version = self.dataset.current()
        key = 'daily/SHFE/2026-01'
        offsets, call = [], Client.call
        def small(client, operation, **values):
            reply = call(client, operation, **values)
            if operation == 'read_dataset_file':
                # Warehouse's chunks are larger than this file; split them to 256 bytes.
                offsets.append(values['offset'])
                reply.data = reply.data[:256]
            return reply
        with patch.object(Client, 'call', small), ThreadPoolExecutor(2) as pool:
            jobs = [pool.submit(self.dataset.read, version, key) for _ in range(2)]
            self.assertEqual([job.result().height for job in jobs], [40, 40])
        self.assertGreater(len(offsets), 1)
        self.assertEqual(offsets.count(0), 1)
        with self.assertRaises(WarehouseError):
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
                        raise WarehouseError('unavailable', 'connection lost')
                    item.data = bytes([item.data[0] ^ 1]) + item.data[1:]
                return item
            with patch.object(Client, 'call', broken), self.assertRaises(WarehouseError):
                self.dataset.read(version, key)
            self.assertFalse(path.exists())
            self.assertEqual(list(path.parent.glob('.download-*')), [])
        self.assertEqual(self.dataset.read(version, key).height, 40)
        wrong = self.warehouse.selected()
        wrong['fingerprint'] = '0' * 64
        with Client(wrong) as client, self.assertRaisesRegex(WarehouseError, 'fingerprint'):
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
        run.limits.max_contract_lots.text = '10'
        run.limits.max_working_orders = 10
        run.periods_per_year, run.zero_risk_free_rate = 252, True
        return request

    def test_real_children_sweeps_rolling_factors_and_charts_read_the_admitted_version(self):
        async def scenario():
            service = Service('lab', self.lab, Log(), self.warehouse)
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
                run.id = 'factor-1'
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
                warmup_query = Request(id=1, build_warmup=dict(
                    dataset_version=self.original.identity,
                    instruments=[dict(instrument=dict(exchange='SHFE', symbol='rb2605'),
                                      contract='SHFE/rb/2026-05')],
                    period=Period.PERIOD_DAY, max_bars=10))
                warmup_reply = await service.reply(warmup_query)
                self.assertEqual(warmup_reply.WhichOneof('result'), 'build_warmup', warmup_reply)
                self.assertEqual(warmup_reply.build_warmup.dataset_version, self.original.identity)
                self.assertEqual(len(warmup_reply.build_warmup.bars), 10)
                self.assertEqual(len(warmup_reply.build_warmup.sha256), 64)
                self.publish_incomplete()
                query.get_chart_bars.dataset_version = ''
                self.assertLess(len((await service.reply(query)).get_chart_bars.bars), 30)
            finally:
                for process in service.backtests.values():
                    process.terminate()
                service.reads.shutdown(wait=True)
        asyncio.run(scenario())

    def test_protocol_hash_mismatch_is_rejected_and_resource_exhausted_retries(self):
        with patch('asterion_lab.warehouse_client.WAREHOUSE_PROTOCOL', '000000000000'):
            with self.assertRaises(WarehouseError) as failed:
                with Client(self.warehouse.selected()) as client:
                    client.call('get_dataset_summary')
            self.assertEqual(failed.exception.code, 'conflict')
            self.assertIn('warehouse protocol differs', str(failed.exception))

        real_send = Client._send
        attempts = {'count': 0}
        def flaky(client, operation, **values):
            if operation == 'get_dataset_summary' and attempts['count'] < 2:
                attempts['count'] += 1
                raise WarehouseError('resource_exhausted', 'busy')
            return real_send(client, operation, **values)

        with patch.object(Client, '_send', flaky), patch('asterion_lab.warehouse_client.time.sleep'):
            with Client(self.warehouse.selected()) as client:
                summary = client.call('get_dataset_summary')
        self.assertEqual(attempts['count'], 2)
        self.assertEqual(summary.dataset.version, self.original.identity)

    async def finished(self, service):
        deadline = time.monotonic() + 40
        while service.backtests or service.waiting or service.rolling:
            self.assertLess(time.monotonic(), deadline, 'research children did not finish')
            await asyncio.sleep(.05)


if __name__ == '__main__':
    unittest.main()
