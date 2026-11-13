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
import shutil
from pathlib import Path
import tempfile

from asterion.common_pb2 import ErrorCode, Period, Side, Offset
from asterion.lab_pb2 import (AdmittedFactor, AdmittedRun, FactorResult, Request, Response,
                              RollingRun, RunState)

from asterion_lab import backtest, chart, repository, package, rolling
from asterion_lab.runs import IDENTITY, Runs
from asterion_lab.warehouse import write_durably
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
STATES = {"running": RunState.RUN_STATE_RUNNING, "succeeded": RunState.RUN_STATE_SUCCEEDED,
          "failed": RunState.RUN_STATE_FAILED, "stopped": RunState.RUN_STATE_STOPPED,
          "interrupted": RunState.RUN_STATE_INTERRUPTED}


def error_code(name: str) -> int:
    return ErrorCode.Value(f"ERROR_CODE_{name.upper()}") if f"ERROR_CODE_{name.upper()}" in \
        ErrorCode.keys() else ErrorCode.ERROR_CODE_OPERATION_FAILED


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
    # A sweep's run that has not started has no record yet.
    if "symbols" in row:
        item.label = row["label"]
        item.symbols.extend(row["symbols"])
        item.first_day, item.last_day = row["begin_day"], row["end_day"]
        item.period = next(key for key, value in PERIODS.items()
                           if value == ("day" if row["period"] == "day" else int(row["period"][:-1])))
        parameters(item.params, row["params"])
    return item


def parameters(target, values: dict):
    """Parameter values into a ParameterValue map; a rolling run's candidate
    lists are not values of one run."""
    fields = {bool: "flag", int: "integer", float: "number", str: "text"}
    for name, value in values.items():
        if type(value) in fields:
            setattr(target[name], fields[type(value)], value)


class Service:
    def __init__(self, name: str, directory: Path, log, warehouse=None):
        self.name = name
        self.log = log
        self.initialized = False
        self.directory = directory
        self.warehouse = warehouse
        self.reads = ThreadPoolExecutor(max_workers=2, thread_name_prefix="warehouse-read")
        self.runs = Runs(directory)
        # Runs a stopped service left running ended with it.
        self.runs.interrupt()
        self.prune_sources()
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
        """The evaluation with the research dataset version it reads and its own
        copy of the repository's code."""
        if not IDENTITY.fullmatch(request.id) or self.factor_path(request.id).exists():
            raise ValueError("factor result id is invalid or already used")
        admitted = AdmittedFactor(evaluation=request)
        admitted.pinned.dataset_version = self.data_version("", ("daily",))
        admitted.source, revision = self.copy_source(
            request.repository, f"factor-{request.id}",
            f"factors/{request.factor.partition('.')[0]}.py", repository.lab_provides)
        admitted.pinned.commit, admitted.pinned.changes = revision.commit, revision.changes
        return admitted

    def factor_path(self, identity):
        return self.directory / "factors" / f"{identity}.binpb"

    def save_factor(self, admitted, result):
        module = admitted.evaluation.factor.partition(".")[0]
        result.id = admitted.evaluation.id
        result.evaluation.CopyFrom(admitted.evaluation)
        result.code = (Path(admitted.source) / "factors" / f"{module}.py").read_text()
        result.evaluated_ms = int(datetime.datetime.now().timestamp() * 1000)
        write_durably(self.factor_path(result.id), result.SerializeToString())

    def factor_results(self):
        results = []
        for path in (self.directory / "factors").glob("*.binpb"):
            result = FactorResult()
            result.ParseFromString(path.read_bytes())
            del result.days[:], result.groups[:]
            results.append(result)
        return sorted(results, key=lambda item: item.evaluated_ms, reverse=True)

    def factor_result(self, identity):
        path = self.factor_path(identity)
        if not IDENTITY.fullmatch(identity) or not path.exists():
            raise ValueError(f"no factor result {identity}")
        result = FactorResult()
        result.ParseFromString(path.read_bytes())
        return result

    async def on_list_factor_results(self, _, result):
        result.results.extend(await self.read(self.factor_results))

    async def on_get_factor_result(self, request, result):
        result.CopyFrom(await self.read(self.factor_result, request.id))

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
        """Evaluates a factor in a child process, keeps its result and replies
        with it, within the time the Terminal waits for a reply."""
        admitted = await self.read(self.pin_factor, request)
        process = await asyncio.create_subprocess_exec(
            *repository.child("asterion_lab.factor"), str(self.directory), cwd=admitted.source,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        try:
            output, error = await asyncio.wait_for(
                process.communicate(admitted.SerializeToString()), FACTOR_SECONDS)
        except TimeoutError:
            process.kill()
            await process.wait()
            raise ValueError(f"the factor took longer than {FACTOR_SECONDS} s; "
                             "evaluate fewer symbols or a shorter range")
        if process.returncode != 0:
            lines = error.decode(errors="replace").strip().splitlines()
            raise ValueError(lines[-1] if lines else f"the factor exited with {process.returncode}")
        result.ParseFromString(output)
        await self.read(self.save_factor, admitted, result)

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

    def copy_source(self, root, name, entry, provided=repository.trading_provides):
        """A copy of the code `entry` needs for one admission alone, and the
        repository's revision."""
        (self.directory / "sources").mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=f"{name}-", dir=self.directory / "sources"))
        return str(destination), repository.snapshot(Path(root), destination, entry, provided)

    def with_source(self, admitted, name):
        """The run with its strategy's code copied for it alone."""
        module = admitted.backtest.strategy.partition(".")[0]
        admitted.source, taken = self.copy_source(admitted.backtest.repository, name,
                                                  f"strategies/{module}.py")
        admitted.pinned.commit, admitted.pinned.changes = taken.commit, taken.changes
        return admitted

    async def launch(self, request, module="asterion_lab.backtest", source=None):
        """Starts the child of an AdmittedRun, or with `source` of a RollingRun."""
        identity = request.id if source else request.backtest.id
        config = backtest.config_of(request.base if source else request)
        plan = backtest.plan(config, config.dataset_version)
        if source:
            members = await self.read(self.runs.sweep, request.sweep_id)
            plan = rolling.plan(config, config.dataset_version, request.sweep_id, members,
                                request.walk_forward.training_days,
                                request.walk_forward.validation_days, [])
        process = None
        try:
            process = await asyncio.create_subprocess_exec(
                *repository.child(module), str(self.directory), cwd=source or request.source,
                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE)
            process.stdin.write(request.SerializeToString())
            process.stdin.close()
        except Exception as error:
            if process is not None:
                if process.returncode is None:
                    process.kill()
                await process.communicate()
            self.finish_child(identity, plan, "failed", str(error))
            raise
        self.backtests[identity] = process
        started = asyncio.get_running_loop().create_future()
        asyncio.create_task(self.watch(identity, process, started, plan))
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
            _, run = self.waiting[0]
            try:
                await self.launch(run)
            except Exception as error:
                self.log.write("error", MODULE, "backtest.not_started", run=run.backtest.id,
                               message=str(error))
            self.waiting.popleft()
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
            except Exception as error:
                self.log.write("error", MODULE, "backtest.not_started", run=rolling.request.id,
                               message=str(error))
            finally:
                del self.rolling[sweep]

    async def on_start_backtest(self, request, _):
        await self.checked_run(request)
        admitted = await self.read(self.pin_backtest, request)
        admitted = await asyncio.to_thread(self.with_source, admitted, f"run-{request.id}")
        # Accepted once recorded: whoever asked can read the run at once.
        error = await asyncio.shield(await self.launch(admitted))
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
            # Only the swept values: they name the run within its sweep.
            item.params.clear()
            parameters(item.params, row["params"])
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

    def finish_child(self, identity, plan, state, error):
        row = next((row for row in self.runs.list() if row["id"] == identity), None)
        if row is None:
            self.runs.fail_start(identity, plan, error, state)
        elif row["state"] == "running":
            self.runs.end(identity, state, error)
        else:
            return row["state"]
        return state

    async def watch(self, identity, process, started, plan):
        """Resolves `started` with "" once the child has written its run's
        record, or with why it ended before it could."""
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

        if await process.stdout.readline() == b"started\n":
            started.set_result("")
        _, error = await asyncio.gather(drain(process.stdout), tail(process.stderr))
        error = error.decode(errors="replace").strip()
        await process.wait()
        code = process.returncode
        message = "cancelled" if code < 0 else error or f"exited with {code} before completing its run"
        try:
            state = self.finish_child(identity, plan, "interrupted" if code < 0 else "failed", message)
            if state == "succeeded":
                self.log.write("info", MODULE, "backtest.finished", run=identity)
            else:
                self.log.write("warning", MODULE, "backtest.failed", run=identity, exit_code=code,
                               stack=error)
        except Exception as failure:
            message = str(failure)
            self.log.write("error", MODULE, "backtest.record_failed", run=identity, message=message)
        finally:
            if not started.done():
                started.set_result(message.splitlines()[-1])
            del self.backtests[identity]
            await self.pump()

    def prune_sources(self):
        """Removes code copies no record refers to: those of deleted records, and
        of admissions that never recorded. Only at start, when nothing is queued."""
        kept = {f"run-{row['id']}" for row in self.runs.list()}
        kept |= {f"sweep-{sweep}" for sweep in self.runs.sweep_ids()}
        kept |= {f"factor-{path.stem}" for path in (self.directory / "factors").glob("*.binpb")}
        # Each copy is named "<kind>-<record id>-<random>".
        for path in (self.directory / "sources").glob("*"):
            if path.name.split("-", 1)[0] in ("run", "sweep", "factor") \
                    and path.name.rsplit("-", 1)[0] not in kept:
                shutil.rmtree(path)

    def remove_sources(self, *names):
        for name in names:
            for path in (self.directory / "sources").glob(f"{name}-*"):
                if path.name.rsplit("-", 1)[0] == name:
                    shutil.rmtree(path)

    async def on_delete_run(self, request, _):
        async with self.admission:
            rows = {row["id"]: row for row in await self.read(self.runs.list)}
            row = rows.get(request.id)
            if row is None:
                raise ValueError(f"no run {request.id}")
            sweep = row["sweep"]
            members = [request.id]
            if sweep:
                members = [item["id"] for item in await self.read(self.runs.sweep, sweep)]
                members.append(f"{sweep}-rolling")
            if set(members) & set(self.backtests) or sweep and (
                    sweep in self.rolling or any(item[0] == sweep for item in self.waiting)):
                raise ValueError("a running or waiting run cannot be deleted")
            await asyncio.to_thread(self.runs.delete, members)
            await asyncio.to_thread(self.remove_sources, *[f"run-{item}" for item in members],
                                    *([f"sweep-{sweep}"] if sweep else []))

    async def on_delete_factor_result(self, request, _):
        path = self.factor_path(request.id)
        if not IDENTITY.fullmatch(request.id) or not path.exists():
            raise ValueError(f"no factor result {request.id}")
        path.unlink()
        await asyncio.to_thread(self.remove_sources, f"factor-{request.id}")

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
                            time_ms=fill["time_ns"] // 1_000_000,
                            trading_day=fill["trading_day"].isoformat(), contract=fill["contract"],
                            side=Side.Value(f"SIDE_{fill['side'].upper()}"),
                            offset=Offset.Value(f"OFFSET_{fill['offset'].upper()}"),
                            lots=decimal(fill["quantity"]), price=decimal(fill["price"]),
                            fee=decimal(fill["fee"]))

    async def on_rename_run(self, request, _):
        await asyncio.to_thread(self.runs.rename, request.id, request.label)

    async def on_list_runs(self, _, result):
        for row in await self.read(self.runs.list):
            summary(result.runs.add(), row)
