"""Requests of the lab service. One asyncio loop owns the connections; reads of
fixed Warehouse versions run on a small thread pool."""
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

from asterion.common_pb2 import ErrorCode, Period, Side, Offset
from asterion.lab_pb2 import (AdmittedFactor, AdmittedRun, Adjustment, NightSession, Request,
                              Response, RollingRun, RunState)

from asterion_lab import backtest, chart, dominant, repository, package
from asterion_lab.runs import IDENTITY, Runs
from asterion_lab.sessions import Sessions, holiday_between
from asterion_lab.warehouse_client import Client, Dataset, WarehouseError

MODULE = "lab-service"
# Keep child deadlines inside Terminal's 65 s reply budget. QuantStats imports
# native libraries on demand; their first load needs more time than a factor.
FACTOR_SECONDS = 25
REPORT_SECONDS = 60


def text(value) -> str:
    """An exact decimal without trailing zeros."""
    return format(Decimal(value).normalize(), "f")


def decimal(value):
    """A common.Decimal, or None to leave the field absent."""
    return None if value is None else {"text": text(value)}


# Chart periods as chart.py names them.
PERIODS = {Period.PERIOD_MINUTES_1: 1, Period.PERIOD_MINUTES_5: 5, Period.PERIOD_MINUTES_10: 10,
           Period.PERIOD_MINUTES_15: 15, Period.PERIOD_MINUTES_30: 30, Period.PERIOD_MINUTES_60: 60,
           Period.PERIOD_DAY: "day", Period.PERIOD_WEEK: "week", Period.PERIOD_MONTH: "month",
           Period.PERIOD_QUARTER: "quarter", Period.PERIOD_YEAR: "year"}
NIGHTS = {"held": NightSession.NIGHT_SESSION_HELD, "none": NightSession.NIGHT_SESSION_NONE,
          "holiday_eve": NightSession.NIGHT_SESSION_HOLIDAY_EVE,
          "unknown": NightSession.NIGHT_SESSION_UNKNOWN}
STATES = {"running": RunState.RUN_STATE_RUNNING, "succeeded": RunState.RUN_STATE_SUCCEEDED,
          "failed": RunState.RUN_STATE_FAILED, "stopped": RunState.RUN_STATE_STOPPED,
          "interrupted": RunState.RUN_STATE_INTERRUPTED}
BEIJING = datetime.timezone(datetime.timedelta(hours=8))


def beijing_ms(stamp: str) -> int:
    """Unix milliseconds of a "YYYY-MM-DD HH:MM" Beijing time."""
    moment = datetime.datetime.strptime(stamp, "%Y-%m-%d %H:%M").replace(tzinfo=BEIJING)
    return int(moment.timestamp() * 1000)


def error_code(name: str) -> int:
    return ErrorCode.Value(f"ERROR_CODE_{name.upper()}") if f"ERROR_CODE_{name.upper()}" in \
        ErrorCode.keys() else ErrorCode.ERROR_CODE_OPERATION_FAILED


def split_product(product: str):
    exchange, _, name = product.partition("/")
    if not exchange or not name:
        raise ValueError(f"a product is EXCHANGE/product: {product}")
    return exchange, name


@dataclass
class Rolling:
    request: object  # RollingRun
    candidates: int
    starting: bool = False


def summary(item, row):
    """A run's state and metrics as a RunSummary."""
    item.id = row["id"]
    if row["state"]:
        item.strategy = row["strategy"]
        item.pinned.dataset_version = row["dataset_version"]
        item.pinned.commit, item.pinned.changes = row["commit"], row["changes"]
        item.state, item.error = STATES[row["state"]], row["error"]
        item.started_ms = row["started_ns"] // 1_000_000
        if row["finished_ns"]:
            item.finished_ms = row["finished_ns"] // 1_000_000
    for name, value in row["metrics"].items():
        if value is not None:
            item.metrics[name] = value
    item.sweep_id = row.get("sweep", "")
    return item


class Service:
    def __init__(self, name: str, directory: Path, log, warehouse=None):
        self.name = name
        self.log = log
        self.initialized = False
        self.directory = directory
        self.warehouse = warehouse
        self.reads = ThreadPoolExecutor(max_workers=2, thread_name_prefix="warehouse-read")
        self.sessions = Sessions()
        self.runs = Runs(directory)
        # Runs a stopped service left running ended with it.
        self.runs.interrupt()
        self.backtests = {}  # run id -> its child process, while it runs
        self.waiting = collections.deque()  # (sweep, AdmittedRun) not started yet
        self.processes = max(1, (os.cpu_count() or 2) - 1)
        self.rolling = {}  # sweep -> its RollingRun, until every candidate has ended
        # Checks, version selection and child startup form one admission. Reads
        # stay independent while filesystem/process work awaits.
        self.admission = asyncio.Lock()

    async def reply(self, request: Request) -> Response:
        """The result of one call, in the field of the call's name."""
        response = Response(id=request.id)
        call = request.WhichOneof("call")
        try:
            if call is None:
                raise ValueError("request has no call")
            handler = getattr(self, f"on_{call}")
            result = getattr(response, call)
            if call in ("start_backtest", "start_sweep", "cancel_run", "cancel_sweep"):
                async with self.admission:
                    await handler(getattr(request, call), result)
            else:
                await handler(getattr(request, call), result)
            result.SetInParent()
        except WarehouseError as error:
            response.error.code, response.error.message = error_code(error.code), str(error)
        except ValueError as error:
            response.error.code, response.error.message = ErrorCode.ERROR_CODE_INVALID_REQUEST, str(error)
        except Exception as error:
            self.log.write("error", MODULE, "request.failed", operation=call, message=str(error))
            response.error.code = ErrorCode.ERROR_CODE_OPERATION_FAILED
            response.error.message = str(error)
        return response

    async def read(self, function, *arguments):
        return await asyncio.get_running_loop().run_in_executor(self.reads, function, *arguments)

    async def on_build_package(self, request, result):
        result.CopyFrom(await self.read(package.build, request))

    async def on_get_health(self, _, result):
        result.ready = self.initialized

    def dataset(self):
        return Dataset(self.directory, self.warehouse)

    def data_version(self, identity, tables):
        """The research dataset's version, which must hold each of `tables`."""
        dataset = self.dataset()
        version = dataset.version(identity) if identity else dataset.current()
        if not version.identity:
            raise ValueError("Warehouse has no research dataset yet")
        for table in tables:
            if not any(key.startswith(table + "/") for key in version.manifest["partitions"]):
                raise ValueError(f"research version {version.identity} has no {table} partitions")
        return version.identity

    def pin_factor(self, request):
        return AdmittedFactor(evaluation=request,
                              dataset_version=self.data_version("", ("daily",)))

    def pin_backtest(self, request, version=""):
        """The run with the research dataset version it reads."""
        tables = ("daily",) if request.period == Period.PERIOD_DAY else ("daily", "minutes")
        admitted = AdmittedRun(backtest=request)
        admitted.pinned.dataset_version = self.data_version(version, tables)
        return admitted

    async def on_get_dataset_summary(self, _, result):
        def read_summary():
            if self.warehouse is None:
                raise ValueError("configure the remote Warehouse address first")
            with Client(self.warehouse.selected()) as client:
                return client.call("get_dataset_summary")
        result.CopyFrom(await self.read(read_summary))

    def chart_bars(self, request):
        exchange = request.contract.split("/")[0]
        period = PERIODS.get(request.period)
        if period is None:
            raise ValueError("a chart needs a period")
        if period in chart.CALENDAR:
            warehouse = self.dataset()
            version = warehouse.version(request.dataset_version) if request.dataset_version else warehouse.current()
            rows = warehouse.query(version, """
                select day, open, high, low, close, volume from daily
                where contract = ? order by day""", ("daily",), (request.contract,),
                exchange=exchange)
            return version, chart.calendar_bars(rows, period)
        warehouse = self.dataset()
        version = warehouse.version(request.dataset_version) if request.dataset_version else warehouse.current()
        rows = warehouse.query(version, """
            select day, start_ms, partial, open, high, low, close, volume from minutes
            where contract = ? order by start_ms""", ("minutes",), (request.contract,),
            contract=request.contract)
        return version, chart.minute_bars(rows, period)

    async def on_get_chart_bars(self, request, result):
        version, bars = await self.read(self.chart_bars, request)
        result.dataset_version = version.identity
        for bar, indicator in zip(bars, chart.macd(bar["close"] for bar in bars)):
            item = result.bars.add(end_ms=bar["time_ms"], trading_day=bar["trading_day"].isoformat(),
                                   partial=bar["partial"],
                                   **{name: decimal(bar[name]) for name in
                                      ("open", "high", "low", "close", "volume")})
            if indicator:
                item.macd_diff, item.macd_signal, item.macd_histogram = indicator

    def stored_contracts(self, only="", pinned=""):
        warehouse = self.dataset()
        version = warehouse.version(pinned) if pinned else warehouse.current()
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

    async def on_list_stored_contracts(self, request, result):
        version, found, names = await self.read(self.stored_contracts, request.contract,
                                                request.dataset_version)
        result.dataset_version = version.identity
        for contract in sorted(found):
            item = result.contracts.add(contract=contract, name=names.get(contract) or "")
            for table, coverage in (("daily", item.daily), ("minutes", item.minutes)):
                if table in found[contract]:
                    coverage.first_day, coverage.last_day, coverage.days = found[contract][table]

    def dominant(self, request):
        if request.adjustment == Adjustment.ADJUSTMENT_UNSPECIFIED:
            raise ValueError("a dominant series needs an adjustment")
        exchange, product = split_product(request.product)
        warehouse = self.dataset()
        version = warehouse.current()
        # The first day's dominant month is read from the day before it.
        rows = warehouse.query(version, """
            select contract, day, settlement, open_interest, close, open, high, low from daily
            where exchange = ? and product = ? and day between ?::date - 31 and ?::date
            order by day""", ("daily",),
            (exchange, product, request.first_day, request.last_day),
            exchange=exchange)
        months = dominant.months_of([row[:5] for row in rows])
        plan = dominant.plan(months, request.traded)
        bars = {(row[0], row[1].isoformat()): row for row in rows}
        return version, months, plan, bars

    async def on_get_dominant_series(self, request, series):
        version, months, plan, bars = await self.read(self.dominant, request)
        series.dataset_version = version.identity
        segments = plan.segments
        for index, segment in enumerate(segments):
            if index and segment.begin >= request.first_day:
                series.rolls.add(trading_day=segment.begin,
                                 from_contract=months[segments[index - 1].month].contract,
                                 to_contract=months[segment.month].contract,
                                 ratio=decimal(plan.ratios[index]))
        current = -1
        for day in plan.days:
            while current + 1 < len(segments) and segments[current + 1].begin <= day:
                current += 1
            if current < 0 or day < request.first_day:
                continue
            segment = segments[current]
            contract = months[segment.month].contract
            factor = segment.factor if request.adjustment == Adjustment.ADJUSTMENT_RATIO else Decimal(1)
            row = bars.get((contract, day))
            values = {}
            if row is not None:
                for name, value in (("settlement", row[2]), ("close", row[4]), ("open", row[5]),
                                    ("high", row[6]), ("low", row[7])):
                    if value is not None:
                        values[name] = decimal(dominant.rounded(Decimal(value) * factor))
            series.days.add(trading_day=day, contract=contract, **values)

    def open_days(self, exchange, begin, end):
        warehouse = self.dataset()
        version = warehouse.current()
        rows = warehouse.query(version, """
            select day from calendar where exchange = ? and open
            and day between ?::date - 31 and ?::date order by day""", ("calendar",),
            (exchange, begin, end), exchange=exchange)
        return [row[0] for row in rows]

    async def on_get_trading_sessions(self, request, reply):
        exchange, product = split_product(request.product)
        days = await self.read(self.open_days, exchange, request.first_day, request.last_day)
        reply.reviewed_day = self.sessions.reviewed
        named = set()
        for previous, trading_day in zip(days, days[1:]):
            if trading_day.isoformat() < request.first_day:
                continue
            day = self.sessions.day(exchange, product, trading_day)
            night = self.sessions.night(exchange, product, previous,
                                        holiday_between(previous, trading_day))
            item = reply.days.add(trading_day=trading_day.isoformat(),
                                  night_session=NIGHTS[night.state])
            for begin, end in day.segments:
                item.day.add(start_ms=beijing_ms(begin), end_ms=beijing_ms(end))
            if night.state == "held":
                item.night.start_ms, item.night.end_ms = beijing_ms(night.begin), beijing_ms(night.end)
            item.source_ids.extend(sorted(set(day.sources) | set(night.sources)))
            named |= set(item.source_ids)
        for identity in sorted(named):
            source = self.sessions.sources[identity]
            reply.sources.add(id=identity, title=source["title"], url=source["url"],
                              published_day=source["published"], note=source["note"])

    async def on_list_strategies(self, request, result):
        result.CopyFrom(await asyncio.to_thread(repository.list_strategies, request.path))

    async def on_read_source(self, request, result):
        result.CopyFrom(repository.read_source(Path(request.repository), request.path))

    async def on_save_source(self, request, _):
        repository.write_source(Path(request.source.repository), request.source.path,
                                request.content, request.create)

    async def on_delete_source(self, request, _):
        repository.delete_source(Path(request.repository), request.path)

    async def on_evaluate_factor(self, request, result):
        """Evaluates a factor in a child process and replies with its result,
        within the time the Terminal waits for a reply."""
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        request = await self.read(self.pin_factor, request)
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
        result.ParseFromString(output)

    async def on_get_run_report(self, request, reply):
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
        reply.quantstats_version = result["version"]
        reply.figures.update(result["figures"])

    def check_run_id(self, identity):
        if not IDENTITY.fullmatch(identity) or identity in self.backtests \
                or self.runs.used(identity) \
                or any(rolling.request.id == identity for rolling in self.rolling.values()):
            raise ValueError("run id is invalid or already used")

    async def checked_run(self, request):
        """A run that can start: an unused id, valid settings, a Git work tree."""
        self.check_run_id(request.id)
        backtest.config_of(AdmittedRun(backtest=request))
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        await asyncio.to_thread(lambda: repository.revision(repository.checked(root)))

    def with_source(self, admitted, name):
        """The run with the repository's code copied for it alone."""
        (self.directory / "sources").mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=f"{name}-", dir=self.directory / "sources"))
        taken = repository.snapshot(Path(admitted.backtest.repository), destination)
        admitted.source = str(destination)
        admitted.pinned.commit, admitted.pinned.changes = taken.commit, taken.changes
        return admitted

    async def launch(self, request, module="asterion_lab.backtest", source=None):
        """Starts the child of an AdmittedRun, or with `source` of a RollingRun."""
        identity = request.id if source else request.backtest.id
        process = await asyncio.create_subprocess_exec(
            *repository.child(module), str(self.directory), cwd=source or request.source,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        process.stdin.write(request.SerializeToString())
        process.stdin.close()
        self.backtests[identity] = process
        started = asyncio.get_running_loop().create_future()
        asyncio.create_task(self.watch(identity, process, started))
        self.log.write("info", MODULE, "backtest.started", run=identity)
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
                self.log.write("error", MODULE, "backtest.not_started", run=run.backtest.id,
                               message=str(error))
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

    async def on_start_backtest(self, request, _):
        await self.checked_run(request)
        admitted = await self.read(self.pin_backtest, request)
        admitted = await asyncio.to_thread(self.with_source, admitted, f"run-{request.id}")
        # Accepted once recorded: whoever asked can read the run at once.
        error = await (await self.launch(admitted))
        if error:
            raise ValueError(error)

    async def on_start_sweep(self, request, _):
        if not IDENTITY.fullmatch(request.id) or any(item[0] == request.id for item in self.waiting):
            raise ValueError("sweep id is invalid or already used")
        names = [item.name for item in request.values]
        if not names or len(set(names)) != len(names) or any(not item.values for item in request.values):
            raise ValueError("a sweep gives each of its parameters one or more values")
        count = math.prod(len(item.values) for item in request.values)
        if count > 10000:
            raise ValueError(f"a sweep runs at most 10000 combinations, not {count}")
        combinations = list(itertools.product(*[list(item.values) for item in request.values]))
        backtest.config_of(AdmittedRun(backtest=request.base))
        base = await self.read(self.pin_backtest, request.base)
        base = await asyncio.to_thread(self.with_source, base, f"sweep-{request.id}")
        runs, planned = [], []
        for n, values in enumerate(combinations):
            run = AdmittedRun()
            run.CopyFrom(base)
            run.backtest.id = f"{request.id}-{n}"
            for name, value in zip(names, values):
                run.backtest.params[name].CopyFrom(value)
            runs.append(run)
            planned.append((run.backtest.id, {name: getattr(value, value.WhichOneof("value"))
                                              for name, value in zip(names, values)}))
        await self.checked_run(runs[0].backtest)
        for run in runs[1:]:
            self.check_run_id(run.backtest.id)
            backtest.config_of(run)
        rolling = None
        if request.HasField("walk_forward"):
            if request.base.holdout_day or len(runs) < 2:
                raise ValueError("rolling validation compares two or more candidates and takes no holdout")
            if request.walk_forward.training_days < 20 or request.walk_forward.validation_days < 1:
                raise ValueError("rolling validation needs at least 20 training days and one validation day")
            rolling = RollingRun(id=f"{request.id}-rolling", sweep_id=request.id, base=base,
                                 walk_forward=request.walk_forward)
            self.check_run_id(rolling.id)
        await asyncio.to_thread(self.runs.plan_sweep, request.id, planned)
        if request.processes:
            self.processes = min(request.processes, os.cpu_count() or 1)
        if rolling:
            self.rolling[request.id] = Rolling(rolling, len(runs))
        self.waiting.extend((request.id, run) for run in runs)
        await self.start_waiting()

    async def on_cancel_sweep(self, request, _):
        members = {row["id"] for row in await self.read(self.runs.sweep, request.id)}
        members.add(f"{request.id}-rolling")
        self.waiting = collections.deque(item for item in self.waiting if item[0] != request.id)
        self.rolling.pop(request.id, None)
        for identity in members & set(self.backtests):
            self.backtests[identity].terminate()

    async def on_get_sweep(self, request, reply):
        rows = await self.read(self.runs.sweep, request.id)
        waiting = any(item[0] == request.id for item in self.waiting)
        best = None
        for row in rows:
            item = summary(reply.runs.add(), row)
            for name, value in row["params"].items():
                field = {bool: "flag", int: "integer", float: "number", str: "text"}[type(value)]
                setattr(item.params[name], field, value)
            sharpe = row["metrics"].get("development_sharpe")
            if sharpe is not None and (best is None or sharpe > best[1]):
                best = (row["id"], sharpe)
        reply.selected_id = best[0] if best else ""
        rolling = next((row for row in await self.read(self.runs.list)
                        if row["id"] == f"{request.id}-rolling"), None)
        if rolling:
            summary(reply.rolling, rolling)
        members = {row["id"] for row in rows} | {f"{request.id}-rolling"}
        reply.active = bool(waiting or members & set(self.backtests) or request.id in self.rolling)

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
                       stack=error)
        # A child stopped from outside could not record how it ended.
        if any(row["id"] == identity and row["state"] == "running" for row in self.runs.list()):
            self.runs.end(identity, "interrupted", "cancelled" if code < 0 else error)

    async def on_cancel_run(self, request, _):
        process = self.backtests.get(request.id)
        if process is None:
            raise ValueError("no such run is running")
        process.terminate()

    def run_detail(self, identity):
        row = next((row for row in self.runs.list() if row["id"] == identity), None)
        if row is None:
            raise ValueError(f"no run {identity}")
        record = (self.runs.root / identity / "run.json").read_text()
        if row["state"] != "succeeded":
            return row, record, None
        return row, record, {name: self.runs.table(identity, name)
                             for name in ("equity", "settlements", "fills")}

    async def on_get_run(self, request, reply):
        row, record, tables = await self.read(self.run_detail, request.id)
        summary(reply.summary, row)
        reply.record = record
        if tables is None:
            return
        for point in tables["equity"].iter_rows(named=True):
            reply.equity.add(time_ms=point["time_ns"] // 1_000_000, equity=decimal(point["equity"]),
                             settlement=point["event"] == "settlement")
        for day in tables["settlements"].iter_rows(named=True):
            reply.days.add(trading_day=day["trading_day"].isoformat(),
                           settled_ms=day["time_ns"] // 1_000_000, equity=decimal(day["equity"]), balance=decimal(day["balance"]),
                           fees=decimal(day["fees"]), margin=decimal(day["margin"]))
        for fill in tables["fills"].iter_rows(named=True):
            reply.fills.add(id=fill["fill"], order_id=fill["order"],
                            time_ms=fill["time_ns"] // 1_000_000, contract=fill["contract"],
                            side=Side.Value(f"SIDE_{fill['side'].upper()}"),
                            offset=Offset.Value(f"OFFSET_{fill['offset'].upper()}"),
                            lots=decimal(fill["quantity"]), price=decimal(fill["price"]),
                            fee=decimal(fill["fee"]))

    async def on_list_runs(self, _, result):
        for row in await self.read(self.runs.list):
            summary(result.runs.add(), row)
