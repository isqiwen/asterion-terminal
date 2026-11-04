"""Requests of the lab service. One asyncio loop owns the connections; reads of
fixed Market versions run on a small thread pool."""
import asyncio
import collections
from dataclasses import dataclass
import datetime
import itertools
import json
import math
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import os
from pathlib import Path
import tempfile
import time

from asterion.lab_pb2 import Request, Response, RollingRun

from asterion_lab import backtest, chart, dominant, repository, package
from asterion_lab.runs import IDENTITY, Runs
from asterion_lab.sessions import Sessions, holiday_between
from asterion_lab.version import VERSION
from asterion_lab.market import Client, Dataset, MarketError

MODULE = "lab-service"
# Keep child deadlines inside Terminal's 65 s reply budget. QuantStats imports
# native libraries on demand; their first load needs more time than a factor.
FACTOR_SECONDS = 25
REPORT_SECONDS = 60


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
    def __init__(self, name: str, directory: Path, log, market=None):
        self.name = name
        self.log = log
        self.started = time.monotonic()
        self.initialized = False
        self.directory = directory
        self.market = market
        self.reads = ThreadPoolExecutor(max_workers=2, thread_name_prefix="warehouse-read")
        self.sessions = Sessions()
        self.runs = Runs(directory)
        # Runs a stopped service left running ended with it.
        self.runs.interrupt()
        self.backtests = {}  # run id -> its child process, while it runs
        self.waiting = collections.deque()  # (sweep, BacktestRun) not started yet
        self.processes = max(1, (os.cpu_count() or 2) - 1)
        self.rolling = {}  # sweep -> its RollingRun, until every candidate has ended
        # Checks, version selection and child startup form one admission. Reads
        # stay independent while filesystem/process work awaits.
        self.admission = asyncio.Lock()

    async def reply(self, request: Request) -> Response:
        response = Response(version=1, service_id=self.name, correlation_id=request.correlation_id)
        try:
            if request.version != 1 or request.service_id != self.name:
                raise ValueError("request is for another service or protocol version")
            body = request.WhichOneof("body")
            if body is None:
                raise ValueError("request has no operation")
            handler = getattr(self, f"on_{body}")
            if body in ("backtest", "sweep", "cancel_run", "cancel_sweep"):
                async with self.admission:
                    await handler(getattr(request, body), response)
            else:
                await handler(getattr(request, body), response)
        except MarketError as error:
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

    async def on_strategy_package(self, request, response):
        response.strategy_package.CopyFrom(await self.read(package.build, request))

    async def on_health(self, _, response):
        health = response.health
        health.instance_id = self.name
        health.process_id = str(os.getpid())
        health.version = VERSION
        health.uptime_ms = int((time.monotonic() - self.started) * 1000)
        health.initialized = self.initialized

    def dataset(self):
        return Dataset(self.directory, self.market)

    def data_version(self, identity, tables):
        """The research dataset's version, which must hold each of `tables`."""
        dataset = self.dataset()
        version = dataset.version(identity) if identity else dataset.current()
        if not version.identity:
            raise ValueError("Market has no research dataset yet")
        for table in tables:
            if not any(key.startswith(table + "/") for key in version.manifest["partitions"]):
                raise ValueError(f"research version {version.identity} has no {table} partitions")
        return version.identity

    def pin_daily(self, request):
        fixed = type(request)()
        fixed.CopyFrom(request)
        fixed.dataset_version = self.data_version(fixed.dataset_version, ("daily",))
        return fixed

    def pin_backtest(self, request):
        fixed = type(request)()
        fixed.CopyFrom(request)
        tables = ("daily", "minutes") if request.minutes else ("daily",)
        fixed.dataset_version = self.data_version(fixed.dataset_version, tables)
        return fixed

    async def on_summary(self, request, response):
        def read_summary():
            if self.market is None:
                raise ValueError("configure the remote Market address first")
            with Client(self.market.selected()) as client:
                return client.call("get_dataset_summary", provider=Dataset.provider)
        value = await self.read(read_summary)
        response.summary.dataset_version = value.dataset_version
        response.summary.created_ns = value.created_ns
        for table in value.tables:
            response.summary.tables.add(table=table.table, rows=table.rows,
                partitions=table.partitions, first_day=table.first_day, last_day=table.last_day)

    def chart_bars(self, request):
        exchange = request.contract.split("/")[0]
        if request.period in chart.CALENDAR:
            warehouse = self.dataset()
            version = warehouse.version(request.dataset_version) if request.dataset_version else warehouse.current()
            rows = warehouse.query(version, """
                select day, open, high, low, close, volume from daily
                where contract = ? order by day""", ("daily",), (request.contract,),
                exchange=exchange)
            return version, chart.calendar_bars(rows, request.period)
        if not request.period.isdigit():
            raise ValueError(f"no chart period {request.period!r}")
        warehouse = self.dataset()
        version = warehouse.version(request.dataset_version) if request.dataset_version else warehouse.current()
        rows = warehouse.query(version, """
            select day, start_ms, partial, open, high, low, close, volume from minutes
            where contract = ? order by start_ms""", ("minutes",), (request.contract,),
            contract=request.contract)
        return version, chart.minute_bars(rows, int(request.period))

    async def on_chart_bars(self, request, response):
        version, bars = await self.read(self.chart_bars, request)
        reply = response.chart_bars
        reply.dataset_version, reply.contract, reply.period = (
            version.identity, request.contract, request.period)
        for bar, indicator in zip(bars, chart.macd(bar["close"] for bar in bars)):
            item = reply.bars.add(time_ms=bar["time_ms"], trading_day=bar["trading_day"].isoformat(),
                                  partial=bar["partial"],
                                  **{name: text(bar[name]) for name in
                                     ("open", "high", "low", "close", "volume")})
            if indicator:
                item.macd_diff, item.macd_signal, item.macd_histogram = indicator

    def stored_contracts(self, only=""):
        warehouse = self.dataset()
        version = warehouse.current()
        found = {}
        for contract, first, last, days in warehouse.query(version, """
                select contract, min(day), max(day), count(distinct day) from daily
                where ? = '' or contract = ? group by contract""", ("daily",), (only, only),
                exchange=only.split("/")[0]):
            found.setdefault(contract, {})["daily"] = (first.isoformat(), last.isoformat(), days)
        for contract, coverage in version.minute_coverage(only).items():
            found.setdefault(contract, {})["minutes"] = coverage
        names = dict(warehouse.query(version, "select contract, name from contracts",
                                     ("contracts",)))
        return version, found, names

    async def on_stored_contracts(self, request, response):
        version, found, names = await self.read(self.stored_contracts, request.contract)
        reply = response.stored_contracts
        reply.dataset_version = version.identity
        for contract in sorted(found):
            item = reply.contracts.add(contract=contract, name=names.get(contract) or "")
            if "daily" in found[contract]:
                item.daily_first, item.daily_last, item.daily_days = found[contract]["daily"]
            if "minutes" in found[contract]:
                item.minute_first, item.minute_last, item.minute_days = found[contract]["minutes"]

    def dominant(self, request):
        if request.adjustment not in ("none", "ratio"):
            raise ValueError("adjustment is none or ratio")
        warehouse = self.dataset()
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

    def open_days(self, exchange, begin, end):
        warehouse = self.dataset()
        version = warehouse.current()
        rows = warehouse.query(version, """
            select day from calendar where exchange = ? and open
            and day between ?::date - 31 and ?::date order by day""", ("calendar",),
            (exchange, begin, end), exchange=exchange)
        return [row[0] for row in rows]

    async def on_sessions(self, request, response):
        days = await self.read(self.open_days, request.exchange, request.begin_day, request.end_day)
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

    async def on_source(self, request, response):
        response.source.CopyFrom(repository.read_source(Path(request.repository), request.path))

    async def on_save_source(self, request, response):
        response.source.CopyFrom(repository.write_source(
            Path(request.repository), request.path, request.content, request.create))

    async def on_delete_source(self, request, response):
        repository.delete_source(Path(request.repository), request.path)
        response.source.path = request.path

    async def on_factor(self, request, response):
        """Evaluates a factor in a child process and replies with its result,
        within the time the Terminal waits for a reply."""
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        request = await self.read(self.pin_daily, request)
        process = await asyncio.create_subprocess_exec(
            *repository.child("asterion_lab.factor"), str(self.directory), cwd=root,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        try:
            output, error = await asyncio.wait_for(
                process.communicate(request.SerializeToString()), FACTOR_SECONDS)
        except TimeoutError:
            process.kill()
            await process.wait()
            raise ValueError(f"the factor took longer than {FACTOR_SECONDS} s; "
                             "evaluate fewer symbols or a shorter range")
        if process.returncode != 0:
            lines = error.decode(errors="replace").strip().splitlines()
            raise ValueError(lines[-1] if lines else f"the factor exited with {process.returncode}")
        response.factor.ParseFromString(output)

    async def on_run_report(self, request, response):
        """QuantStats figures of a finished run, computed in a child process."""
        if not IDENTITY.fullmatch(request.id):
            raise ValueError("run id is invalid")
        process = await asyncio.create_subprocess_exec(
            *repository.child("asterion_lab.report"), str(self.directory), request.id,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            output, error = await asyncio.wait_for(process.communicate(), REPORT_SECONDS)
        except TimeoutError:
            process.kill()
            await process.wait()
            raise ValueError(f"QuantStats took longer than {REPORT_SECONDS} s")
        if process.returncode != 0:
            lines = error.decode(errors="replace").strip().splitlines()
            raise ValueError(lines[-1] if lines else f"QuantStats exited with {process.returncode}")
        result = json.loads(output)
        reply = response.run_report
        reply.id, reply.quantstats_version = request.id, result["version"]
        reply.figures.update(result["figures"])

    def check_run_id(self, identity):
        if not IDENTITY.fullmatch(identity) or identity in self.backtests \
                or self.runs.used(identity) \
                or any(rolling.request.id == identity for rolling in self.rolling.values()):
            raise ValueError("run id is invalid or already used")

    async def checked_run(self, request):
        """A run that can start: an unused id, valid settings, a Git work tree."""
        self.check_run_id(request.id)
        backtest.config_of(request)
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        await asyncio.to_thread(lambda: repository.revision(repository.checked(root)))

    def with_source(self, request, name):
        """The request with the repository's code copied for it alone."""
        fixed = type(request)()
        fixed.CopyFrom(request)
        (self.directory / "sources").mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=f"{name}-", dir=self.directory / "sources"))
        taken = repository.snapshot(Path(request.repository), destination)
        fixed.source, fixed.commit, fixed.changes = str(destination), taken.commit, taken.changes
        return fixed

    async def launch(self, request, module="asterion_lab.backtest", source=None):
        process = await asyncio.create_subprocess_exec(
            *repository.child(module), str(self.directory), cwd=source or request.source,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        process.stdin.write(request.SerializeToString())
        process.stdin.close()
        self.backtests[request.id] = process
        started = asyncio.get_running_loop().create_future()
        asyncio.create_task(self.watch(request.id, process, started))
        self.log.write("info", MODULE, "backtest.started", run=request.id)
        return started

    async def pump(self):
        async with self.admission:
            await self.start_waiting()

    async def start_waiting(self):
        """Starts waiting runs while fewer than the allowed number run, and a
        sweep's rolling validation once all its candidates have ended.
        Called with the admission lock held."""
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
                                      rolling.request.base.source)
            finally:
                del self.rolling[sweep]

    async def on_backtest(self, request, response):
        await self.checked_run(request)
        request = await self.read(self.pin_backtest, request)
        request = await asyncio.to_thread(self.with_source, request, f"run-{request.id}")
        # Accepted once recorded: whoever asked can read the run at once.
        error = await (await self.launch(request))
        if error:
            raise ValueError(error)
        response.accepted.SetInParent()

    async def on_sweep(self, request, response):
        if not IDENTITY.fullmatch(request.id) or any(item[0] == request.id for item in self.waiting):
            raise ValueError("sweep id is invalid or already used")
        names = [item.name for item in request.values]
        if not names or len(set(names)) != len(names) or any(not item.values for item in request.values):
            raise ValueError("a sweep gives each of its parameters one or more values")
        count = math.prod(len(item.values) for item in request.values)
        if count > 10000:
            raise ValueError(f"a sweep runs at most 10000 combinations, not {count}")
        combinations = list(itertools.product(*[list(item.values) for item in request.values]))
        backtest.config_of(request.base)
        base = await self.read(self.pin_backtest, request.base)
        base = await asyncio.to_thread(self.with_source, base, f"sweep-{request.id}")
        runs, planned = [], []
        for n, values in enumerate(combinations):
            run = type(request.base)()
            run.CopyFrom(base)
            run.id = f"{request.id}-{n}"
            for name, value in zip(names, values):
                run.params[name].CopyFrom(value)
            runs.append(run)
            planned.append((run.id, {name: getattr(value, value.WhichOneof("value"))
                                     for name, value in zip(names, values)}))
        await self.checked_run(runs[0])
        for run in runs[1:]:
            self.check_run_id(run.id)
            backtest.config_of(run)
        rolling = None
        if request.HasField("walk_forward"):
            if request.base.holdout_day or len(runs) < 2:
                raise ValueError("rolling validation compares two or more candidates and takes no holdout")
            if request.walk_forward.training_days < 20 or request.walk_forward.validation_days < 1:
                raise ValueError("rolling validation needs at least 20 training days and one validation day")
            rolling = RollingRun(id=f"{request.id}-rolling", sweep=request.id, base=base,
                                 training_days=request.walk_forward.training_days,
                                 validation_days=request.walk_forward.validation_days)
            self.check_run_id(rolling.id)
        await asyncio.to_thread(self.runs.plan_sweep, request.id, planned)
        if request.processes:
            self.processes = min(request.processes, os.cpu_count() or 1)
        if rolling:
            self.rolling[request.id] = Rolling(rolling, len(runs))
        self.waiting.extend((request.id, run) for run in runs)
        await self.start_waiting()
        response.accepted.SetInParent()

    async def on_cancel_sweep(self, request, response):
        members = {row["id"] for row in await self.read(self.runs.sweep, request.id)}
        members.add(f"{request.id}-rolling")
        self.waiting = collections.deque(item for item in self.waiting if item[0] != request.id)
        self.rolling.pop(request.id, None)
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
        members = {row["id"] for row in rows} | {f"{request.id}-rolling"}
        reply.active = bool(reply.waiting or members & set(self.backtests)
                            or request.id in self.rolling)

    async def watch(self, identity, process, started):
        """Resolves `started` with "" once the child has written its run's
        record, or with why it ended before it could."""
        recorded = await process.stdout.readline() == b"started\n"
        if recorded:
            started.set_result("")
        # What the strategy prints is read and dropped, so the child never blocks on
        # it; only the end of standard error is kept, for the failure message.
        async def drain(stream):
            while await stream.read(65536):
                pass

        async def tail(stream):
            kept = b""
            while chunk := await stream.read(65536):
                kept = (kept + chunk)[-65536:]
            return kept

        _, error = await asyncio.gather(drain(process.stdout), tail(process.stderr))
        error = error.decode(errors="replace").strip()
        code = await process.wait()
        if not recorded:
            started.set_result(error.splitlines()[-1] if error else f"exited with {code}")
        del self.backtests[identity]
        await self.pump()
        if code == 0:
            self.log.write("info", MODULE, "backtest.finished", run=identity)
            return
        self.log.write("warning", MODULE, "backtest.failed", run=identity, exit_code=code,
                       message=error.splitlines()[-1] if error else "")
        # A child stopped from outside could not record how it ended.
        if any(row["id"] == identity and row["state"] == "running" for row in self.runs.list()):
            self.runs.end(identity, "interrupted", "cancelled" if code < 0 else error)

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
