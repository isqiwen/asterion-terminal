"""Requests of the lab service. One asyncio loop owns the connections; reads of
the warehouse run on a small thread pool and acquisition on its own thread."""
import asyncio
import collections
from dataclasses import dataclass
import datetime
import itertools
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import os
from pathlib import Path
import time

from asterion.lab_pb2 import Request, Response, RollingRun

from asterion_lab.acquisition import Acquisition
from asterion_lab import backtest, crosscheck, dominant, repository
from asterion_lab.runs import IDENTITY, Runs
from asterion_lab.sessions import Sessions, holiday_between
from asterion_lab.tushare import Budget, Tushare
from asterion_lab.sources.base import SourceError
from asterion_lab.version import VERSION
from asterion_lab.warehouse import Warehouse, SCHEMAS, frame
from asterion_lab.sources.catalog import PLUGINS, plugin
from asterion_lab.sources.base import Query
from asterion_lab.sources.akshare import Akshare

MODULE = "lab-service"


def text(value) -> str:
    """An exact decimal without trailing zeros; empty when absent."""
    if value is None:
        return ""
    return format(Decimal(value).normalize(), "f")


@dataclass
class Rolling:
    request: object  # RollingRun
    candidates: int
    starting: bool = False


def summary(item, row):
    """A run's state and metrics as a RunSummary."""
    item.id = row["id"]
    if row["state"]:
        item.strategy, item.commit, item.changes = row["strategy"], row["commit"], row["changes"]
        item.dataset_version, item.state, item.error = row["dataset_version"], row["state"], row["error"]
        item.started_ns, item.finished_ns = row["started_ns"], row["finished_ns"] or 0
    for name, value in row["metrics"].items():
        if value is not None:
            item.metrics[name] = value
    item.sweep = row.get("sweep", "")
    return item


class Service:
    def __init__(self, name: str, directory: Path, log):
        self.name = name
        self.log = log
        self.started = time.monotonic()
        self.initialized = False
        self.directory = directory
        self.warehouse = Warehouse(directory)
        self.acquisitions = {"tushare": Acquisition(self.warehouse, log)}
        self.sources = {}  # provider -> (credential, Budget), in memory only
        self.reads = ThreadPoolExecutor(max_workers=2, thread_name_prefix="warehouse-read")
        self.sessions = Sessions()
        self.runs = Runs(directory)
        # Runs a stopped service left running ended with it.
        self.runs.interrupt()
        self.backtests = {}  # run id -> its child process, while it runs
        self.waiting = collections.deque()  # (sweep, BacktestRun) not started yet
        self.processes = max(1, (os.cpu_count() or 2) - 1)
        self.rolling = {}  # sweep -> its RollingRun, until every candidate has ended

    async def reply(self, request: Request) -> Response:
        response = Response(version=1, service_id=self.name, correlation_id=request.correlation_id)
        try:
            if request.version != 1 or request.service_id != self.name:
                raise ValueError("request is for another service or protocol version")
            body = request.WhichOneof("body")
            if body is None:
                raise ValueError("request has no operation")
            handler = getattr(self, f"on_{body}")
            await handler(getattr(request, body), response)
        except SourceError as error:
            response.error.code, response.error.message = error.code, str(error)
        except ValueError as error:
            response.error.code, response.error.message = "invalid_request", str(error)
        except Exception as error:
            self.log.write("error", MODULE, "request.failed", operation=request.WhichOneof("body"),
                           message=str(error))
            response.error.code, response.error.message = "operation_failed", str(error)
        return response

    async def read(self, function, *arguments):
        return await asyncio.get_running_loop().run_in_executor(self.reads, function, *arguments)

    async def on_health(self, _, response):
        health = response.health
        health.instance_id = self.name
        health.process_id = str(os.getpid())
        health.version = VERSION
        health.uptime_ms = int((time.monotonic() - self.started) * 1000)
        health.initialized = self.initialized
        health.configured_sources.extend(sorted(self.sources))

    def warehouse_for(self, provider, table=None):
        implementation = plugin(provider)
        if table is not None and table not in implementation.tables:
            raise SourceError("unsupported", f"{provider} does not provide {table}")
        # Existing Tushare partitions stay at their owned path; never migrate user data.
        return self.warehouse if provider == "tushare" else Warehouse(
            self.directory / "sources" / provider, provider)

    def source(self, provider, cancelled=lambda: False):
        implementation = plugin(provider)
        config = self.sources.get(provider)
        if implementation.requires_credential:
            if config is None:
                raise SourceError("invalid_credential", "enter or save the Tushare credential first")
            credential, budget = config
            return implementation(credential, budget, cancelled)
        if implementation.requires_endpoint:
            if config is None:
                raise SourceError("invalid_request", "configure the recorder endpoint first")
            return implementation(*config, cancelled=cancelled)
        return implementation(cancelled=cancelled)

    async def on_sources(self, _, response):
        for identity, implementation in PLUGINS.items():
            response.sources.sources.add(id=identity, tables=implementation.tables,
                exchanges=implementation.exchanges,
                requires_credential=implementation.requires_credential,
                requires_endpoint=implementation.requires_endpoint,
                configured=identity in self.sources or not (
                    implementation.requires_credential or implementation.requires_endpoint))
        response.sources.SetInParent()

    async def on_configure_source(self, request, response):
        implementation = plugin(request.provider)
        if implementation.requires_credential:
            budget = Budget(request.requests_per_minute)
            implementation(request.credential, budget)
            self.sources[request.provider] = (request.credential, budget)
        elif implementation.requires_endpoint:
            if request.credential:
                raise ValueError("recorder does not accept a credential")
            implementation(request.host, request.port)
            self.sources[request.provider] = (request.host, request.port)
        else:
            raise ValueError("this source needs no configuration")
        self.log.write("info", MODULE, "source.configured", provider=request.provider)
        response.accepted.SetInParent()

    async def on_source_coverage(self, request, response):
        source = self.source(request.provider)
        value = await self.read(source.coverage)
        response.source_coverage.provider = request.provider
        response.source_coverage.known = value["known"]
        response.source_coverage.days.extend(value["days"])
        response.source_coverage.note = value["note"]

    async def on_source_read(self, request, response):
        query = Query(request.table, request.exchange, request.day, request.contract)
        source = self.source(request.provider)
        source.check(query)
        result = await self.read(source.read, query)
        data = frame(request.table, result.rows)  # Same shape and precision as acquisition.
        reply = response.source_table
        reply.provider, reply.table = request.provider, request.table
        reply.columns.extend(data.columns)
        for row in data.iter_rows():
            item = reply.rows.add()
            for value in row:
                cell = item.cells.add()
                if value is not None:
                    cell.value = text(value) if isinstance(value, Decimal) else str(value)
        for observation in result.observations:
            reply.observations.add(**{key: observation[key] for key in (
                "instance", "number", "first_received_ms", "last_received_ms", "ticks", "end")})

    async def on_acquire(self, request, response):
        if any(request.id in owner.tasks for owner in self.acquisitions.values()):
            raise ValueError("acquisition id already exists")
        source = self.source(request.provider)
        if request.contract and request.table != "minutes":
            raise ValueError("daily and settlement acquisitions cover the whole exchange")
        if request.table not in source.tables or request.table not in ("daily", "minutes", "settlement"):
            raise SourceError("unsupported", "this source does not support the requested acquisition")
        exchanges = list(request.exchanges) or list(source.exchanges)
        for exchange in exchanges:
            source.check(Query(request.table, exchange, request.begin_day, request.contract))
        if request.table == "minutes" and not request.contract:
            raise ValueError("recorded minutes require a contract")
        if request.provider not in self.acquisitions:
            self.acquisitions[request.provider] = Acquisition(self.warehouse_for(request.provider), self.log)
        def configured_source(cancelled):
            source.cancelled = cancelled
            return source
        self.acquisitions[request.provider].submit(request.id, request.begin_day, request.end_day,
            configured_source, exchanges=exchanges,
            refresh=request.refresh, provider=request.provider, table=request.table,
            contract=request.contract)
        response.accepted.SetInParent()

    async def on_cancel(self, request, response):
        owner = next((acquisition for acquisition in self.acquisitions.values()
                      if request.id in acquisition.tasks), None)
        if owner is None:
            raise ValueError("unknown acquisition")
        owner.cancel(request.id)
        response.accepted.SetInParent()

    async def on_tasks(self, _, response):
        for acquisition in self.acquisitions.values():
            for task in acquisition.tasks.values():
                response.tasks.tasks.add(
                    id=task.identity, begin_day=task.begin, end_day=task.end, state=task.state,
                    days_total=task.days_total, days_done=task.days_done, days_empty=task.days_empty,
                    requests=task.requests, error_code=task.error_code, error=task.error,
                    dataset_version=task.version, started_ns=task.started_ns,
                    finished_ns=task.finished_ns, provider=task.provider, table=task.table,
                    contract=task.contract)
        response.tasks.SetInParent()

    def summary(self, provider):
        warehouse = self.warehouse_for(provider)
        version = warehouse.current()
        tables = []
        for table in SCHEMAS:
            day = "list_date" if table == "contracts" else "day"
            (rows, first, last), = warehouse.query(
                version, f"select count(*), min({day}), max({day}) from {table}", (table,))
            partitions = sum(key.split("/")[0] == table for key in version.manifest["partitions"])
            tables.append((table, rows, partitions, first, last))
        return version, tables

    async def on_summary(self, request, response):
        version, tables = await self.read(self.summary, request.provider)
        summary = response.summary
        summary.dataset_version = version.identity
        summary.created_ns = version.manifest["created_ns"]
        for table, rows, partitions, first, last in tables:
            summary.tables.add(table=table, rows=rows, partitions=partitions,
                               first_day=first.isoformat() if first else "",
                               last_day=last.isoformat() if last else "")

    def coverage(self, provider):
        warehouse = self.warehouse_for(provider)
        version = warehouse.current()
        rows = warehouse.query(version, """
            with d as (select exchange, product, strftime(day, '%Y-%m') as month,
                              count(distinct day) as days from daily group by all),
                 o as (select exchange, strftime(day, '%Y-%m') as month, count(*) as open_days
                       from calendar where open group by all)
            select d.exchange, d.product, d.month, d.days, coalesce(o.open_days, 0)
            from d left join o using (exchange, month) order by 1, 2, 3""", ("daily", "calendar"))
        return version, rows

    async def on_coverage(self, request, response):
        version, rows = await self.read(self.coverage, request.provider)
        coverage = response.coverage
        coverage.dataset_version = version.identity
        current = None
        for exchange, product, month, days, open_days in rows:
            if current is None or (current.exchange, current.product) != (exchange, product):
                current = coverage.products.add(exchange=exchange, product=product)
            current.months.add(month=month, days=days, open_days=open_days)

    def daily_bars(self, contract, begin, end, provider):
        warehouse = self.warehouse_for(provider, "daily")
        version = warehouse.current()
        rows = warehouse.query(version, """
            select day, open, high, low, close, settlement, volume, open_interest from daily
            where contract = ? and day between ?::date and ?::date order by day""",
            ("daily",), (contract, begin, end), exchange=contract.split("/")[0])
        return version, rows

    async def on_daily_bars(self, request, response):
        version, rows = await self.read(self.daily_bars, request.contract, request.begin_day,
                                        request.end_day, request.provider)
        bars = response.daily_bars
        bars.dataset_version = version.identity
        bars.contract = request.contract
        for day, *values in rows:
            bars.bars.add(day=day.isoformat(), **dict(zip(
                ("open", "high", "low", "close", "settlement", "volume", "open_interest"),
                map(text, values))))

    def dominant(self, request):
        if request.adjustment not in ("none", "ratio"):
            raise ValueError("adjustment is none or ratio")
        warehouse = self.warehouse_for(request.provider, "daily")
        version = warehouse.current()
        # The first day's dominant month is read from the day before it.
        rows = warehouse.query(version, """
            select contract, day, settlement, open_interest, close, open, high, low from daily
            where exchange = ? and product = ? and day between ?::date - 31 and ?::date
            order by day""", ("daily",),
            (request.exchange, request.product, request.begin_day, request.end_day),
            exchange=request.exchange)
        months = dominant.months_of([row[:5] for row in rows])
        plan = dominant.plan(months, request.traded)
        bars = {(row[0], row[1].isoformat()): row for row in rows}
        return version, months, plan, bars

    async def on_dominant(self, request, response):
        version, months, plan, bars = await self.read(self.dominant, request)
        series = response.dominant
        series.dataset_version = version.identity
        series.adjustment = request.adjustment
        segments = plan.segments
        for index, segment in enumerate(segments):
            if index and segment.begin >= request.begin_day:
                series.rolls.add(day=segment.begin, from_contract=months[segments[index - 1].month].contract,
                                 to_contract=months[segment.month].contract,
                                 ratio=text(plan.ratios[index]))
        current = -1
        for day in plan.days:
            while current + 1 < len(segments) and segments[current + 1].begin <= day:
                current += 1
            if current < 0 or day < request.begin_day:
                continue
            segment = segments[current]
            contract = months[segment.month].contract
            factor = segment.factor if request.adjustment == "ratio" else Decimal(1)
            row = bars.get((contract, day))
            values = {}
            if row is not None:
                for name, value in (("settlement", row[2]), ("close", row[4]), ("open", row[5]),
                                    ("high", row[6]), ("low", row[7])):
                    values[name] = text(None if value is None else dominant.rounded(Decimal(value) * factor))
            series.days.add(day=day, contract=contract, **values)

    def open_days(self, exchange, begin, end, provider="tushare"):
        warehouse = self.warehouse_for(provider, "calendar")
        version = warehouse.current()
        rows = warehouse.query(version, """
            select day from calendar where exchange = ? and open
            and day between ?::date - 31 and ?::date order by day""", ("calendar",),
            (exchange, begin, end), exchange=exchange)
        return [row[0] for row in rows]

    async def on_sessions(self, request, response):
        days = await self.read(self.open_days, request.exchange, request.begin_day, request.end_day, request.provider)
        reply = response.sessions
        reply.reviewed = self.sessions.reviewed
        named = set()
        for previous, trading_day in zip(days, days[1:]):
            if trading_day.isoformat() < request.begin_day:
                continue
            day = self.sessions.day(request.exchange, request.product, trading_day)
            night = self.sessions.night(request.exchange, request.product, previous,
                                        holiday_between(previous, trading_day))
            item = reply.days.add(trading_day=trading_day.isoformat(), day_known=day.known,
                                  night_state=night.state)
            for begin, end in day.segments:
                item.day.add(begin=begin, end=end)
            if night.state == "held":
                item.night.begin, item.night.end = night.begin, night.end
            item.sources.extend(sorted(set(day.sources) | set(night.sources)))
            named |= set(item.sources)
        for identity in sorted(named):
            source = self.sessions.sources[identity]
            reply.sources.add(id=identity, title=source["title"], url=source["url"],
                              published=source["published"], note=source["note"])

    async def on_strategies(self, request, response):
        response.strategies.CopyFrom(await asyncio.to_thread(repository.list_strategies,
                                                             request.repository))

    async def checked_run(self, request):
        """A run that can start: an unused id, valid settings, a Git work tree."""
        if not IDENTITY.fullmatch(request.id) or request.id in self.backtests \
                or (self.runs.root / request.id).exists():
            raise ValueError("run id is invalid or already used")
        backtest.config_of(request)
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        await asyncio.to_thread(lambda: repository.revision(repository.checked(root)))

    async def launch(self, request, module="asterion_lab.backtest", repository_path=None):
        process = await asyncio.create_subprocess_exec(
            *repository.child(module), str(self.directory), cwd=repository_path or request.repository,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE)
        process.stdin.write(request.SerializeToString())
        process.stdin.close()
        self.backtests[request.id] = process
        asyncio.create_task(self.watch(request.id, process))
        self.log.write("info", MODULE, "backtest.started", run=request.id)

    async def pump(self):
        """Starts waiting runs while fewer than the allowed number run, and a
        sweep's rolling validation once all its candidates have ended."""
        while self.waiting and len(self.backtests) < self.processes:
            _, run = self.waiting.popleft()
            try:
                await self.launch(run)
            except Exception as error:
                self.log.write("error", MODULE, "backtest.not_started", run=run.id, message=str(error))
        for sweep, rolling in list(self.rolling.items()):
            members = {f"{sweep}-{n}" for n in range(rolling.candidates)}
            if rolling.starting or any(item[0] == sweep for item in self.waiting) \
                    or members & set(self.backtests):
                continue
            # Kept until its process runs, so the sweep never looks finished before.
            rolling.starting = True
            try:
                states = {row["state"] for row in await self.read(self.runs.sweep, sweep)}
                if states != {"succeeded"}:
                    self.log.write("warning", MODULE, "rolling.skipped", sweep=sweep,
                                   message="not every candidate succeeded")
                else:
                    await self.launch(rolling.request, "asterion_lab.rolling",
                                      rolling.request.base.repository)
            finally:
                del self.rolling[sweep]

    async def on_backtest(self, request, response):
        await self.checked_run(request)
        await self.launch(request)
        response.accepted.SetInParent()

    async def on_sweep(self, request, response):
        if not IDENTITY.fullmatch(request.id) or any(item[0] == request.id for item in self.waiting):
            raise ValueError("sweep id is invalid or already used")
        names = [item.name for item in request.values]
        if not names or len(set(names)) != len(names) or any(not item.values for item in request.values):
            raise ValueError("a sweep gives each of its parameters one or more values")
        combinations = list(itertools.product(*[list(item.values) for item in request.values]))
        if len(combinations) > 10000:
            raise ValueError(f"a sweep runs at most 10000 combinations, not {len(combinations)}")
        runs, planned = [], []
        for n, values in enumerate(combinations):
            run = type(request.base)()
            run.CopyFrom(request.base)
            run.id = f"{request.id}-{n}"
            for name, value in zip(names, values):
                run.params[name].CopyFrom(value)
            runs.append(run)
            planned.append((run.id, {name: getattr(value, value.WhichOneof("value"))
                                     for name, value in zip(names, values)}))
        await self.checked_run(runs[0])
        for run in runs[1:]:
            backtest.config_of(run)
        rolling = None
        if request.HasField("walk_forward"):
            if request.base.holdout_day or len(runs) < 2:
                raise ValueError("rolling validation compares two or more candidates and takes no holdout")
            if request.walk_forward.training_days < 20 or request.walk_forward.validation_days < 1:
                raise ValueError("rolling validation needs at least 20 training days and one validation day")
            rolling = RollingRun(id=f"{request.id}-rolling", sweep=request.id, base=request.base,
                                 training_days=request.walk_forward.training_days,
                                 validation_days=request.walk_forward.validation_days)
        await asyncio.to_thread(self.runs.plan_sweep, request.id, planned)
        if request.processes:
            self.processes = min(request.processes, os.cpu_count() or 1)
        if rolling:
            self.rolling[request.id] = Rolling(rolling, len(runs))
        self.waiting.extend((request.id, run) for run in runs)
        await self.pump()
        response.accepted.SetInParent()

    async def on_cancel_sweep(self, request, response):
        members = {row["id"] for row in await self.read(self.runs.sweep, request.id)}
        self.waiting = collections.deque(item for item in self.waiting if item[0] != request.id)
        for identity in members & set(self.backtests):
            self.backtests[identity].terminate()
        response.accepted.SetInParent()

    async def on_sweep_runs(self, request, response):
        rows = await self.read(self.runs.sweep, request.id)
        reply = response.sweep_runs
        reply.id = request.id
        reply.waiting = sum(item[0] == request.id for item in self.waiting)
        best = None
        for row in rows:
            item = summary(reply.runs.add(), row)
            for name, value in row["params"].items():
                field = {bool: "flag", int: "integer", float: "number", str: "text"}[type(value)]
                setattr(item.params[name], field, value)
            sharpe = row["metrics"].get("development_sharpe")
            if sharpe is not None and (best is None or sharpe > best[1]):
                best = (row["id"], sharpe)
        reply.selected = best[0] if best else ""
        rolling = next((row for row in await self.read(self.runs.list)
                        if row["id"] == f"{request.id}-rolling"), None)
        if rolling:
            summary(reply.rolling, rolling)

    async def watch(self, identity, process):
        error = (await process.stderr.read()).decode(errors="replace").strip()
        code = await process.wait()
        del self.backtests[identity]
        await self.pump()
        if code == 0:
            self.log.write("info", MODULE, "backtest.finished", run=identity)
            return
        self.log.write("warning", MODULE, "backtest.failed", run=identity, exit_code=code,
                       message=error.splitlines()[-1] if error else "")
        # A child stopped from outside could not record how it ended.
        if any(row["id"] == identity and row["state"] == "running" for row in self.runs.list()):
            self.runs.fail(identity, "interrupted", "cancelled" if code < 0 else error)

    async def on_cancel_run(self, request, response):
        process = self.backtests.get(request.id)
        if process is None:
            raise ValueError("no such run is running")
        process.terminate()
        response.accepted.SetInParent()

    def run_detail(self, identity):
        row = next((row for row in self.runs.list() if row["id"] == identity), None)
        if row is None:
            raise ValueError(f"no run {identity}")
        record = (self.runs.root / identity / "run.json").read_text()
        if row["state"] != "succeeded":
            return row, record, None
        return row, record, {name: self.runs.table(identity, name)
                             for name in ("equity", "settlements", "fills")}

    async def on_run(self, request, response):
        row, record, tables = await self.read(self.run_detail, request.id)
        reply = response.run
        summary(reply.summary, row)
        reply.record = record
        if tables is None:
            return
        for point in tables["equity"].iter_rows(named=True):
            reply.equity.add(time_ns=point["time_ns"], equity=text(point["equity"]),
                             settlement=point["event"] == "settlement")
        for day in tables["settlements"].iter_rows(named=True):
            reply.days.add(trading_day=day["trading_day"].isoformat(), time_ns=day["time_ns"],
                           equity=text(day["equity"]), balance=text(day["balance"]),
                           fees=text(day["fees"]), margin=text(day["margin"]))
        for fill in tables["fills"].iter_rows(named=True):
            reply.fills.add(fill=fill["fill"], order=fill["order"], time_ns=fill["time_ns"],
                            contract=fill["contract"], side=fill["side"], offset=fill["offset"],
                            quantity=text(fill["quantity"]), price=text(fill["price"]),
                            fee=text(fill["fee"]))

    async def on_runs(self, _, response):
        for row in await self.read(self.runs.list):
            summary(response.runs.runs.add(), row)
        response.runs.SetInParent()

    DAILY_FIELDS = ("open", "high", "low", "close", "settlement", "previous_settlement", "volume",
                    "open_interest")

    def cross_check(self, request, client):
        begin, end = datetime.date.fromisoformat(request.begin_day), \
            datetime.date.fromisoformat(request.end_day)
        if not 0 <= (end - begin).days <= 30:
            raise ValueError("a cross-check covers 1 to 31 days")
        if request.kind not in ("settlement", "daily"):
            raise ValueError("a cross-check is of settlement or daily")
        version = self.warehouse.current()
        groups = []
        for exchange, products in crosscheck.FIRST_BATCH.items():
            days = [day.isoformat() for day in self.open_days(exchange, request.begin_day,
                                                               request.end_day)
                    if begin <= day <= end]
            for day in days:
                group = crosscheck.Group(exchange, day)
                groups.append(group)
                if request.kind == "settlement":
                    if exchange in crosscheck.UNAVAILABLE_SETTLEMENT:
                        group.unavailable = crosscheck.UNAVAILABLE_SETTLEMENT[exchange]
                        continue
                    first = crosscheck.normalize_tushare(exchange, client.settlement(exchange, day))
                    second = crosscheck.official_settlement(exchange, day)
                    group.note = crosscheck.SETTLEMENT_NOTES[exchange]
                    crosscheck.compare(group, first, second, crosscheck.SETTLEMENT_FIELDS[exchange],
                                       set(products))
                else:
                    if exchange in crosscheck.UNAVAILABLE_DAILY:
                        group.unavailable = crosscheck.UNAVAILABLE_DAILY[exchange]
                        continue
                    rows = self.warehouse.query(version, f"""
                        select contract, {", ".join(self.DAILY_FIELDS)} from daily
                        where exchange = ? and day = ?::date""", ("daily",), (exchange, day),
                        exchange=exchange)
                    first = {row[0]: dict(zip(self.DAILY_FIELDS, row[1:])) for row in rows}
                    second = {row["contract"]: row for row in
                              Akshare().read(Query("daily", exchange, day)).rows}
                    held = {contract.split("/")[1] for contract in first}
                    crosscheck.compare(group, first, second, self.DAILY_FIELDS, held)
        return groups

    async def on_cross_check(self, request, response):
        client = None
        if request.kind == "settlement":
            if "tushare" not in self.sources:
                raise ValueError("enter or save the Tushare credential first")
            credential, budget = self.sources["tushare"]
            client = Tushare(credential, budget)
        groups = await self.read(self.cross_check, request, client)
        reply = response.cross_check
        reply.kind = request.kind
        for group in groups:
            item = reply.groups.add(exchange=group.exchange, day=group.day,
                                    unavailable=group.unavailable, note=group.note,
                                    compared_rows=group.compared_rows,
                                    compared_values=group.compared_values)
            item.only_first.extend(group.only_first)
            item.only_second.extend(group.only_second)
            for contract, name, first, second in group.differences:
                item.differences.add(contract=contract, field=name, first=first, second=second)
