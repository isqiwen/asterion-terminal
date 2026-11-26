"""Requests of the lab service. One asyncio loop owns the connections; reads of
fixed Warehouse versions run on a small thread pool."""
import asyncio
import collections
from concurrent.futures import ThreadPoolExecutor
import datetime
from decimal import Decimal
import json
import os
from pathlib import Path
import shutil
import tempfile
import time

from asterion.common_pb2 import ErrorCode, Offset, Period, Side
from asterion.lab_pb2 import (
    AdmittedFactor,
    AdmittedRun,
    FactorResult,
    ReconcileLive,
    Request,
    Response,
    RunState,
    TaskKind,
)

from asterion_lab import backtest, chart, data as lab_data, optimize, package, repository, rolling, sweeps, warmup, worker
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
PERIODS = {
    Period.PERIOD_MINUTES_1: 1,
    Period.PERIOD_MINUTES_5: 5,
    Period.PERIOD_MINUTES_10: 10,
    Period.PERIOD_MINUTES_15: 15,
    Period.PERIOD_MINUTES_30: 30,
    Period.PERIOD_MINUTES_60: 60,
    Period.PERIOD_DAY: "day",
    Period.PERIOD_WEEK: "week",
    Period.PERIOD_MONTH: "month",
    Period.PERIOD_QUARTER: "quarter",
    Period.PERIOD_YEAR: "year",
}
STATES = {
    "running": RunState.RUN_STATE_RUNNING,
    "succeeded": RunState.RUN_STATE_SUCCEEDED,
    "failed": RunState.RUN_STATE_FAILED,
    "stopped": RunState.RUN_STATE_STOPPED,
    "interrupted": RunState.RUN_STATE_INTERRUPTED,
}
TASK_KINDS = {
    "backtest": TaskKind.TASK_KIND_BACKTEST,
    "sweep": TaskKind.TASK_KIND_SWEEP,
    "batch": TaskKind.TASK_KIND_BATCH,
    "slippage": TaskKind.TASK_KIND_SLIPPAGE_STRESS,
    "rolling": TaskKind.TASK_KIND_ROLLING,
}


def error_code(name: str) -> int:
    return (
        ErrorCode.Value(f"ERROR_CODE_{name.upper()}")
        if f"ERROR_CODE_{name.upper()}" in ErrorCode.keys()
        else ErrorCode.ERROR_CODE_OPERATION_FAILED
    )


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
    item.sweep_size = row.get("sweep_size", 0)
    item.sweep_succeeded = row.get("sweep_succeeded", 0)
    item.sweep_running = row.get("sweep_running", False)
    item.sweep_strategies = row.get("sweep_strategies", 0)
    # A sweep's run that has not started has no record yet.
    if "symbols" in row:
        item.label = row["label"]
        item.symbols.extend(row["symbols"])
        item.first_day, item.last_day = row["begin_day"], row["end_day"]
        item.period = next(
            key
            for key, value in PERIODS.items()
            if value == ("day" if row["period"] == "day" else int(row["period"][:-1]))
        )
        parameters(item.params, row["params"])
        run_settings(item.settings, row["settings"])
    return item


def run_settings(item, settings: dict):
    costs, risk, statistics = settings["costs"], settings["risk"], settings["statistics"]
    item.deposit.text = costs["deposit"]
    item.slippage_ticks = costs["slippage_ticks"]
    item.fee_multiplier.text = str(costs.get("fee_multiplier", "1"))
    for name in ("max_order_lots", "max_gross_lots", "max_contract_lots"):
        getattr(item.limits, name).text = risk[name]
    item.limits.max_working_orders = risk["max_working_orders"]
    for name in ("max_gross_weight", "max_net_weight", "max_margin_ratio", "min_rebalance_weight"):
        getattr(item, name).text = risk.get(name, "0")
    item.periods_per_year = statistics["annual_periods"] or 0
    item.zero_risk_free_rate = statistics["zero_risk_free"]
    item.holdout_day = statistics["holdout_day"]


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
        self.waiting = collections.deque()  # (sweep, AdmittedRun, limit) not started yet
        self.processes = max(1, (os.cpu_count() or 2) - 1)
        self.rolling = {}  # sweep -> its Rolling, until every candidate has ended
        self.bayes = {}  # sweep -> active BayesSweep state across waves
        self.activity = {}  # run or sweep id -> metadata while active
        self.run_task = {}  # child run id -> owning sweep id
        self.recent_tasks = collections.deque(maxlen=12)  # session-completed tasks, newest first
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
            if call in ("start_backtest", "start_sweep", "cancel_run", "cancel_sweep", "build_package"):
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
        result.CopyFrom(await self.read(self.build_package, request))

    def build_package(self, request):
        if not request.run_id:
            return package.build(Path(request.repository), request.strategy)
        if request.repository or request.strategy:
            raise ValueError("choose a backtest or the current strategy directory")
        row = self.runs.get(request.run_id)
        if row is None or row["state"] != "succeeded":
            raise ValueError("a strategy package requires a successful backtest")
        record = self.runs.record(request.run_id)
        if "rolling" in record["statistics"]:
            raise ValueError("choose a single backtest from the rolling validation candidates")
        name = f"sweep-{row['sweep']}" if row["sweep"] else f"run-{row['id']}"
        copies = [
            path
            for path in (self.directory / "sources").glob(f"{name}-*")
            if path.name.rsplit("-", 1)[0] == name
        ]
        if len(copies) != 1:
            raise ValueError(f"the frozen source of run {request.run_id} is missing or ambiguous")
        result = package.build(copies[0], row["strategy"])
        research = result.manifest.research
        research.run_id = request.run_id
        research.dataset_version = row["dataset_version"]
        research.symbols.extend(row["symbols"])
        research.period = next(
            key
            for key, value in PERIODS.items()
            if value == ("day" if row["period"] == "day" else int(row["period"][:-1]))
        )
        parameters(research.params, row["params"])
        research.weight_limits.gross.text = record["risk"]["max_gross_weight"]
        research.weight_limits.net.text = record["risk"]["max_net_weight"]
        if record["risk"].get("max_margin_ratio", "0") != "0":
            research.weight_limits.max_margin_ratio.text = record["risk"]["max_margin_ratio"]
        if record["risk"].get("min_rebalance_weight", "0") != "0":
            research.weight_limits.min_rebalance_weight.text = record["risk"]["min_rebalance_weight"]
        return result

    def build_warmup(self, request):
        return warmup.build_warmup(request, dataset=self.dataset, data_version=self.data_version)

    async def on_build_warmup(self, request, result):
        result.CopyFrom(await self.read(self.build_warmup, request))

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
        is_minutes = request.period not in (Period.PERIOD_UNSPECIFIED, Period.PERIOD_DAY)
        if is_minutes and PERIODS.get(request.period) not in chart.MINUTES:
            raise ValueError("factor evaluation supports daily or 1 to 60 minute bars")
        admitted = AdmittedFactor(evaluation=request)
        tables = ("daily", "minutes") if is_minutes else ("daily",)
        admitted.pinned.dataset_version = self.data_version("", tables)
        if request.symbols and (not request.first_day or not request.last_day):
            exchanges = {s.split("/")[0] for s in request.symbols if s}
            _, found, _ = self.stored_contracts(
                pinned=admitted.pinned.dataset_version,
                exchanges=exchanges,
                include_daily=not is_minutes,
                include_names=False,
            )
            first, last = lab_data.stored_span(found, request.symbols, is_minutes)
            if not admitted.evaluation.first_day:
                admitted.evaluation.first_day = first
            if not admitted.evaluation.last_day:
                admitted.evaluation.last_day = last
        admitted.source, revision = self.copy_source(
            request.repository,
            f"factor-{request.id}",
            f"factors/{request.factor.partition('.')[0]}.py",
        )
        for extra in (*request.extra_factors, *request.orthogonalize_against):
            repository.snapshot(
                Path(request.repository),
                Path(admitted.source),
                f"factors/{extra.factor.partition('.')[0]}.py",
            )
        admitted.pinned.commit, admitted.pinned.changes = revision.commit, revision.changes
        return admitted

    def factor_path(self, identity):
        return self.directory / "factors" / f"{identity}.binpb"

    def save_factor(self, admitted, result):
        modules = []
        for fid in [admitted.evaluation.factor, *(e.factor for e in admitted.evaluation.extra_factors)]:
            mod = fid.partition(".")[0]
            if mod not in modules:
                modules.append(mod)
        result.id = admitted.evaluation.id
        result.evaluation.CopyFrom(admitted.evaluation)
        result.code = "\n\n".join(
            (Path(admitted.source) / "factors" / f"{mod}.py").read_text() for mod in modules
        )
        result.evaluated_ms = int(datetime.datetime.now().timestamp() * 1000)
        write_durably(self.factor_path(result.id), result.SerializeToString())

    def factor_results(self):
        results = []
        for path in (self.directory / "factors").glob("*.binpb"):
            result = FactorResult()
            result.ParseFromString(path.read_bytes())
            del (
                result.days[:],
                result.development_horizons[:],
                result.correlations[:],
            )
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

    def pin_backtest(self, request, version="", resolve_dates=True):
        """The run with the research dataset version it reads."""
        tables = ("daily",) if request.period == Period.PERIOD_DAY else ("daily", "minutes")
        admitted = AdmittedRun(backtest=request)
        admitted.pinned.dataset_version = self.data_version(version, tables)
        if resolve_dates and request.symbols and (not request.first_day or not request.last_day):
            is_minutes = request.period != Period.PERIOD_DAY
            exchanges = {s.split("/")[0] for s in request.symbols if s}
            _, found, _ = self.stored_contracts(
                pinned=admitted.pinned.dataset_version,
                exchanges=exchanges,
                include_daily=not is_minutes,
                include_names=False,
            )
            first, last = lab_data.stored_span(found, request.symbols, is_minutes)
            if not admitted.backtest.first_day:
                admitted.backtest.first_day = first
            if not admitted.backtest.last_day:
                admitted.backtest.last_day = last
        return admitted

    async def on_get_dataset_summary(self, _, result):
        def read_summary():
            if self.warehouse is None:
                raise WarehouseError("not_found", "no Warehouse is configured")
            with Client(self.warehouse.selected()) as client:
                return client.call("get_dataset_summary")

        result.CopyFrom(await self.read(read_summary))

    def chart_bars(self, request):
        exchange = request.contract.split("/")[0]
        period = PERIODS.get(request.period)
        if period is None:
            raise ValueError("a chart needs a period")
        warehouse = self.dataset()
        with warehouse:
            version = (
                warehouse.version(request.dataset_version)
                if request.dataset_version
                else warehouse.current()
            )
            if period in chart.CALENDAR:
                rows = warehouse.query(
                    version,
                    """
                    select day, open, high, low, close, volume from daily
                    where contract = ? order by day""",
                    ("daily",),
                    (request.contract,),
                    exchange=exchange,
                )
                return version, chart.calendar_bars(rows, period)
            rows = warehouse.query(
                version,
                """
                select day, start_ms, partial, open, high, low, close, volume from minutes
                where contract = ? order by start_ms""",
                ("minutes",),
                (request.contract,),
                contract=request.contract,
            )
        return version, chart.minute_bars(rows, period, request.contract)

    async def on_get_chart_bars(self, request, result):
        version, bars = await self.read(self.chart_bars, request)
        result.dataset_version = version.identity
        for bar, indicator in zip(bars, chart.macd(bar["close"] for bar in bars)):
            item = result.bars.add(
                end_ms=bar["time_ms"],
                trading_day=bar["trading_day"].isoformat(),
                partial=bar["partial"],
                **{name: decimal(bar[name]) for name in ("open", "high", "low", "close", "volume")},
            )
            if indicator:
                item.macd_diff, item.macd_signal, item.macd_histogram = indicator

    def stored_contracts(self, only="", pinned="", exchanges=None, include_daily=True, include_names=True):
        warehouse = self.dataset()
        with warehouse:
            version = warehouse.version(pinned) if pinned else warehouse.current()
            found = {}
            if include_daily:
                for contract, coverage in warehouse.daily_coverage(version, only, exchanges).items():
                    found.setdefault(contract, {})["daily"] = coverage
            for contract, coverage in version.minute_coverage(only).items():
                found.setdefault(contract, {})["minutes"] = coverage
            names = {}
            if include_names:
                name_exchanges = (
                    [only.split("/")[0]] if only else (sorted(exchanges) if exchanges else [""])
                )
                for exchange in name_exchanges:
                    names.update(
                        warehouse.query(
                            version,
                            "select contract, name from contracts",
                            ("contracts",),
                            exchange=exchange,
                        )
                    )
        return version, found, names

    async def on_list_stored_contracts(self, request, result):
        version, found, names = await self.read(
            self.stored_contracts, request.contract, request.dataset_version
        )
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
        repository.write_source(
            Path(request.source.repository), request.source.path, request.content, request.create
        )

    async def on_delete_source(self, request, _):
        repository.delete_source(Path(request.repository), request.path)

    async def on_evaluate_factor(self, request, result):
        """Evaluates a factor in a child process, keeps its result and replies
        with it, within the time the Terminal waits for a reply."""
        admitted = await self.read(self.pin_factor, request)
        is_minutes = admitted.evaluation.period not in (Period.PERIOD_UNSPECIFIED, Period.PERIOD_DAY)
        timeout = REPORT_SECONDS if is_minutes else FACTOR_SECONDS
        try:
            try:
                output = await worker.output(
                    "asterion_lab.factor",
                    str(self.directory),
                    cwd=admitted.source,
                    input=admitted.SerializeToString(),
                    timeout=timeout,
                )
            except TimeoutError:
                raise ValueError(
                    f"the factor took longer than {timeout} s; "
                    "evaluate fewer symbols or a shorter range"
                )
            result.ParseFromString(output)
            await self.read(self.save_factor, admitted, result)
        except Exception:
            await asyncio.to_thread(shutil.rmtree, admitted.source, True)
            raise

    async def on_get_run_report(self, request, reply):
        """QuantStats figures of a finished run, computed in a child process."""
        if not IDENTITY.fullmatch(request.id):
            raise ValueError("run id is invalid")
        try:
            output = await worker.output(
                "asterion_lab.report", str(self.directory), request.id, timeout=REPORT_SECONDS
            )
        except TimeoutError:
            raise ValueError(f"QuantStats took longer than {REPORT_SECONDS} s")
        result = json.loads(output)
        reply.quantstats_version = result["version"]
        reply.figures.update(result["figures"])

    def pin_reconcile(self, request):
        if not request.HasField("instance") or not request.steps:
            raise ValueError("reconciliation requires a strategy instance and recorded steps")
        period = PERIODS.get(request.instance.period)
        if period != "day" and period not in chart.MINUTES:
            raise ValueError("unsupported strategy period")
        tables = ("daily",) if period == "day" else ("daily", "minutes")
        version_id = request.dataset_version
        if not version_id and request.run_id:
            version_id = self.runs.record(request.run_id)["data"]["dataset_version"]
        if not version_id and request.instance.HasField("warmup"):
            version_id = request.instance.warmup.dataset_version
        admitted = ReconcileLive()
        admitted.CopyFrom(request)
        admitted.dataset_version = self.data_version(version_id, tables)
        return admitted

    async def on_reconcile_live(self, request, reply):
        admitted = await self.read(self.pin_reconcile, request)
        try:
            output = await worker.output(
                "asterion_lab.reconcile",
                "--rpc",
                str(self.directory),
                input=admitted.SerializeToString(),
                timeout=REPORT_SECONDS,
            )
        except TimeoutError:
            raise ValueError(f"reconciliation took longer than {REPORT_SECONDS} s")
        reply.ParseFromString(output)

    def check_run_id(self, identity):
        if (
            not IDENTITY.fullmatch(identity)
            or identity in self.backtests
            or self.runs.used(identity)
            or any(pending.request.id == identity for pending in self.rolling.values())
        ):
            raise ValueError("run id is invalid or already used")

    async def checked_run(self, request):
        """A run that can start: an unused id, valid settings, a Git work tree."""
        self.check_run_id(request.id)
        backtest.config_of(AdmittedRun(backtest=request))
        root = Path(request.repository)
        if not root.is_absolute() or not root.is_dir():
            raise ValueError(f"strategy repository is not a directory: {root}")
        await asyncio.to_thread(lambda: repository.revision(repository.checked(root)))

    def copy_source(self, root, name, entry):
        """A copy of the code `entry` needs for one admission alone, and the
        repository's revision."""
        (self.directory / "sources").mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=f"{name}-", dir=self.directory / "sources"))
        return str(destination), repository.snapshot(Path(root), destination, entry)

    def with_source(self, admitted, name, others=()):
        """The run with its strategy's code, and that of `others`, copied for it alone."""
        module = admitted.backtest.strategy.partition(".")[0]
        admitted.source, taken = self.copy_source(
            admitted.backtest.repository, name, f"strategies/{module}.py"
        )
        for other in others:
            repository.snapshot(
                Path(admitted.backtest.repository),
                Path(admitted.source),
                f"strategies/{other.partition('.')[0]}.py",
            )
        admitted.pinned.commit, admitted.pinned.changes = taken.commit, taken.changes
        return admitted

    async def launch(self, request, module="asterion_lab.backtest", source=None):
        """Starts the child of an AdmittedRun, or with `source` of a RollingRun."""
        identity = request.id if source else request.backtest.id
        config = backtest.config_of(request.base if source else request)
        plan = backtest.plan(config, config.dataset_version)
        if source:
            members = await self.read(self.runs.sweep, request.sweep_id)
            plan = rolling.plan(
                config,
                config.dataset_version,
                request.sweep_id,
                members,
                request.walk_forward.training_days,
                request.walk_forward.validation_days,
                [],
                request.walk_forward.embargo_days,
                request.walk_forward.objective,
            )
        process = None
        try:
            process = await asyncio.create_subprocess_exec(
                *worker.command(module),
                str(self.directory),
                cwd=source or request.source,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
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
        """Starts waiting runs while fewer than the allowed number run, advances
        active Bayesian optimization sweeps between waves, and starts a sweep's
        rolling validation once all its candidates have ended.
        Called with the admission lock held."""
        while True:
            while self.waiting and len(self.backtests) < self.waiting[0][2]:
                _, run, _ = self.waiting[0]
                try:
                    await self.launch(run)
                except Exception as error:
                    self.log.write(
                        "error", MODULE, "backtest.not_started", run=run.backtest.id, message=str(error)
                    )
                self.waiting.popleft()

            advanced = False
            for sweep, bayes in list(self.bayes.items()):
                if bayes.advancing:
                    continue
                if any(item[0] == sweep for item in self.waiting):
                    continue
                if any(self.run_task.get(r) == sweep for r in self.backtests):
                    continue
                bayes.advancing = True
                try:
                    rows = await self.read(self.runs.sweep, sweep)
                    sweeps.observe_bayes_rows(rows, bayes)
                    remaining = bayes.max_trials - len(bayes.tried)
                    if remaining <= 0:
                        if sweep in self.rolling:
                            self.rolling[sweep].candidates = bayes.next_index
                        del self.bayes[sweep]
                        continue
                    wave = min(bayes.limit, remaining)
                    next_combos = await asyncio.to_thread(
                        optimize.gp_suggest,
                        bayes.axis_coords,
                        list(bayes.observed.items()),
                        wave,
                        tried=bayes.tried,
                        seed=bayes.seed + len(bayes.tried),
                    )
                    if not next_combos:
                        if sweep in self.rolling:
                            self.rolling[sweep].candidates = bayes.next_index
                        del self.bayes[sweep]
                        continue
                    offset, new_runs, new_planned = sweeps.materialize_bayes_wave(
                        sweep, bayes, next_combos
                    )
                    await asyncio.to_thread(self.runs.append_sweep, sweep, offset, new_planned)
                    for run in new_runs:
                        self.run_task[run.backtest.id] = sweep
                    self.waiting.extend((sweep, run, bayes.limit) for run in new_runs)
                    advanced = True
                finally:
                    bayes.advancing = False
            if not advanced:
                break

        for sweep, pending in list(self.rolling.items()):
            if sweep in self.bayes:
                continue
            members = {f"{sweep}-{n}" for n in range(pending.candidates)}
            if (
                pending.starting
                or any(item[0] == sweep for item in self.waiting)
                or members & set(self.backtests)
            ):
                continue
            # Kept until its process runs, so the sweep never looks finished before.
            pending.starting = True
            try:
                states = {row["state"] for row in await self.read(self.runs.sweep, sweep)}
                if states != {"succeeded"}:
                    self.log.write(
                        "warning",
                        MODULE,
                        "rolling.skipped",
                        sweep=sweep,
                        message="not every candidate succeeded",
                    )
                else:
                    await self.launch(
                        pending.request, "asterion_lab.rolling", pending.request.base.source
                    )
            except Exception as error:
                self.log.write(
                    "error", MODULE, "backtest.not_started", run=pending.request.id, message=str(error)
                )
            finally:
                del self.rolling[sweep]

    async def on_start_backtest(self, request, _):
        await self.checked_run(request)
        admitted = await self.read(self.pin_backtest, request)
        admitted = await asyncio.to_thread(self.with_source, admitted, f"run-{request.id}")
        self.activity[request.id] = dict(
            id=request.id,
            kind="backtest",
            strategy=request.strategy,
            symbols=list(admitted.backtest.symbols),
            total=1,
            started_ms=int(time.time() * 1000),
            has_rolling=False,
        )
        # Accepted once recorded: whoever asked can read the run at once.
        try:
            started = await self.launch(admitted)
        except Exception:
            self.activity.pop(request.id, None)
            raise
        error = await asyncio.shield(started)
        if error:
            raise ValueError(error)

    async def on_start_sweep(self, request, _):
        parsed = sweeps.validate_sweep(request, self.waiting)
        symbols, batch, slippages = parsed["symbols"], parsed["batch"], parsed["slippages"]
        base = await self.read(self.pin_backtest, request.base, "", not symbols)
        is_minutes = request.base.period != Period.PERIOD_DAY
        found = None
        if symbols and (not request.base.first_day or not request.base.last_day):
            exchanges = {s.split("/")[0] for s in symbols if s}
            _, found, _ = await self.read(
                self.stored_contracts,
                "",
                base.pinned.dataset_version,
                exchanges,
                not is_minutes,
                False,
            )
        base = await asyncio.to_thread(
            self.with_source, base, f"sweep-{request.id}", [item.strategy for item in batch]
        )
        limit = min(request.processes, os.cpu_count() or 1) if request.processes else self.processes
        runs, planned, bayes_state = sweeps.plan_runs(request, base, found, limit, parsed)
        await self.checked_run(runs[0].backtest)
        for run in runs[1:]:
            self.check_run_id(run.backtest.id)
            backtest.config_of(run)
        rolling_run = sweeps.build_rolling_run(request, base, runs, bayes_state)
        if rolling_run is not None:
            self.check_run_id(rolling_run.id)
        await asyncio.to_thread(self.runs.plan_sweep, request.id, planned)
        kind = "batch" if batch else ("slippage" if slippages else "sweep")
        total_planned = bayes_state.max_trials if bayes_state else len(runs)
        self.activity[request.id] = dict(
            id=request.id,
            kind=kind,
            strategy="" if batch else request.base.strategy,
            symbols=list(symbols or request.base.symbols),
            total=total_planned + (1 if rolling_run else 0),
            started_ms=int(time.time() * 1000),
            has_rolling=bool(rolling_run),
        )
        for run in runs:
            self.run_task[run.backtest.id] = request.id
        if bayes_state:
            self.bayes[request.id] = bayes_state
        if rolling_run:
            self.run_task[rolling_run.id] = request.id
            self.rolling[request.id] = sweeps.Rolling(rolling_run, total_planned)
        self.waiting.extend((request.id, run, limit) for run in runs)
        await self.start_waiting()

    async def on_cancel_sweep(self, request, _):
        members = {row["id"] for row in await self.read(self.runs.sweep, request.id)}
        members.add(f"{request.id}-rolling")
        self.waiting = collections.deque(item for item in self.waiting if item[0] != request.id)
        self.bayes.pop(request.id, None)
        self.rolling.pop(request.id, None)
        running_members = members & set(self.backtests)
        for identity in members - running_members:
            self.run_task.pop(identity, None)
        for identity in running_members:
            self.backtests[identity].terminate()
        if not running_members:
            meta = self.activity.pop(request.id, None)
            if meta is not None:
                info = await self.read(sweeps.sweep_info, self.runs, request.id, meta, 0, 0)
                if info["state"] == "running":
                    info["state"] = "interrupted"
                info["finished_ms"] = int(time.time() * 1000)
                self._record_recent(info)

    async def on_get_sweep(self, request, reply):
        rows = await self.read(self.runs.sweep, request.id)
        waiting = any(item[0] == request.id for item in self.waiting)
        waiting_symbols = {
            item[1].backtest.id: list(item[1].backtest.symbols)
            for item in self.waiting
            if item[0] == request.id
        }
        best = None
        for row in rows:
            item = summary(reply.runs.add(), row)
            if not item.symbols and row["id"] in waiting_symbols:
                item.symbols.extend(waiting_symbols[row["id"]])
            # Only the swept values: they name the run within its sweep.
            item.params.clear()
            parameters(item.params, row["params"])
            sharpe = row["metrics"].get("development_sharpe")
            if sharpe is not None and (best is None or sharpe > best[1]):
                best = (row["id"], sharpe)
        reply.selected_id = best[0] if best else ""
        rolling_row = await self.read(self.runs.get, f"{request.id}-rolling")
        if rolling_row:
            summary(reply.rolling, rolling_row)
        members = {row["id"] for row in rows} | {f"{request.id}-rolling"}
        reply.active = bool(
            waiting
            or members & set(self.backtests)
            or request.id in self.rolling
            or request.id in self.bayes
        )
        if not reply.active:
            pbo_res = await self.read(sweeps.compute_sweep_pbo, self.runs, rows)
            if pbo_res is not None:
                reply.pbo = pbo_res["pbo"]
                if pbo_res["is_oos_rank_corr"] is not None:
                    reply.pbo_rank_corr = pbo_res["is_oos_rank_corr"]

    def finish_child(self, identity, plan, state, error):
        row = self.runs.get(identity)
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
        final_state, final_error = "failed", message
        try:
            state = self.finish_child(identity, plan, "interrupted" if code < 0 else "failed", message)
            final_state = state
            final_error = "" if state == "succeeded" else message
            if state == "succeeded":
                self.log.write("info", MODULE, "backtest.finished", run=identity)
            else:
                self.log.write(
                    "warning", MODULE, "backtest.failed", run=identity, exit_code=code, stack=error
                )
        except Exception as failure:
            message = str(failure)
            final_state, final_error = "failed", message
            self.log.write("error", MODULE, "backtest.record_failed", run=identity, message=message)
        finally:
            if not started.done():
                started.set_result(message.splitlines()[-1])
            del self.backtests[identity]
            await self.pump()
            await self._complete_activity(identity, final_state, final_error)

    def _record_recent(self, info: dict):
        self.recent_tasks = collections.deque(
            (item for item in self.recent_tasks if item["id"] != info["id"]),
            maxlen=self.recent_tasks.maxlen,
        )
        self.recent_tasks.appendleft(info)

    async def _complete_activity(self, identity: str, state: str, error: str):
        sweep_id = self.run_task.pop(identity, None)
        if sweep_id is None:
            meta = self.activity.pop(identity, None)
            if meta is None:
                return
            self._record_recent(
                dict(
                    id=identity,
                    kind=meta["kind"],
                    strategy=meta["strategy"],
                    symbols=meta["symbols"],
                    total=1,
                    done=1,
                    succeeded=1 if state == "succeeded" else 0,
                    failed=0 if state == "succeeded" else 1,
                    running=0,
                    queued=0,
                    started_ms=meta["started_ms"],
                    finished_ms=int(time.time() * 1000),
                    state=state,
                    error=error,
                )
            )
            return
        still_running = any(self.run_task.get(r) == sweep_id for r in self.backtests)
        still_queued = (
            any(item[0] == sweep_id for item in self.waiting)
            or sweep_id in self.rolling
            or sweep_id in self.bayes
        )
        if still_running or still_queued:
            return
        meta = self.activity.pop(sweep_id, None)
        if meta is None:
            return
        info = await self.read(sweeps.sweep_info, self.runs, sweep_id, meta, 0, 0)
        info["finished_ms"] = int(time.time() * 1000)
        self._record_recent(info)

    @staticmethod
    def _write_task(target, info: dict):
        target.id = info["id"]
        target.kind = TASK_KINDS[info["kind"]]
        target.strategy = info["strategy"]
        target.symbols.extend(info["symbols"])
        target.total = info["total"]
        target.done = info["done"]
        target.succeeded = info["succeeded"]
        target.failed = info["failed"]
        target.running = info["running"]
        target.queued = info["queued"]
        target.started_ms = info["started_ms"]
        target.finished_ms = info["finished_ms"]
        target.state = STATES[info["state"]]
        target.error = info["error"]

    async def on_get_activity(self, _, reply):
        reply.running_processes = len(self.backtests)
        reply.max_processes = self.processes
        reply.queued_runs = len(self.waiting)
        for task_id, meta in list(self.activity.items()):
            if meta["kind"] == "backtest":
                running = 1 if task_id in self.backtests else 0
                info = dict(
                    id=task_id,
                    kind="backtest",
                    strategy=meta["strategy"],
                    symbols=meta["symbols"],
                    total=1,
                    done=0,
                    succeeded=0,
                    failed=0,
                    running=running,
                    queued=0,
                    started_ms=meta["started_ms"],
                    finished_ms=0,
                    state="running",
                    error="",
                )
            else:
                running = sum(1 for r in self.backtests if self.run_task.get(r) == task_id)
                bayes_rem = (
                    max(0, self.bayes[task_id].max_trials - len(self.bayes[task_id].tried))
                    if task_id in self.bayes
                    else 0
                )
                queued = (
                    sum(1 for item in self.waiting if item[0] == task_id)
                    + bayes_rem
                    + (1 if task_id in self.rolling else 0)
                )
                info = await self.read(sweeps.sweep_info, self.runs, task_id, meta, running, queued)
            self._write_task(reply.active.add(), info)
        for info in list(self.recent_tasks):
            self._write_task(reply.recent.add(), info)

    def prune_sources(self):
        """Removes code copies no record refers to: those of deleted records, and
        of admissions that never recorded. Only at start, when nothing is queued."""
        kept = {f"run-{identity}" for identity in self.runs.ids()}
        kept |= {f"sweep-{sweep}" for sweep in self.runs.sweep_ids()}
        kept |= {f"factor-{path.stem}" for path in (self.directory / "factors").glob("*.binpb")}
        # Each copy is named "<kind>-<record id>-<random>".
        for path in (self.directory / "sources").glob("*"):
            if (
                path.name.split("-", 1)[0] in ("run", "sweep", "factor")
                and path.name.rsplit("-", 1)[0] not in kept
            ):
                shutil.rmtree(path)

    def remove_sources(self, *names):
        for name in names:
            for path in (self.directory / "sources").glob(f"{name}-*"):
                if path.name.rsplit("-", 1)[0] == name:
                    shutil.rmtree(path)

    async def on_delete_run(self, request, _):
        async with self.admission:
            row = await self.read(self.runs.get, request.id)
            if row is None:
                raise ValueError(f"no run {request.id}")
            sweep = row["sweep"]
            members = [request.id]
            if sweep:
                members = [item["id"] for item in await self.read(self.runs.sweep, sweep)]
                members.append(f"{sweep}-rolling")
            if set(members) & set(self.backtests) or (
                sweep
                and (
                    sweep in self.rolling
                    or sweep in self.bayes
                    or any(item[0] == sweep for item in self.waiting)
                )
            ):
                raise ValueError("a running or waiting run cannot be deleted")
            await asyncio.to_thread(self.runs.delete, members)
            await asyncio.to_thread(
                self.remove_sources,
                *[f"run-{item}" for item in members],
                *([f"sweep-{sweep}"] if sweep else []),
            )
            removed = set(members) | ({sweep} if sweep else set())
            self.recent_tasks = collections.deque(
                (item for item in self.recent_tasks if item["id"] not in removed),
                maxlen=self.recent_tasks.maxlen,
            )

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
        row = self.runs.get(identity)
        if row is None:
            raise ValueError(f"no run {identity}")
        record = (self.runs.root / identity / "run.json").read_text()
        if row["state"] != "succeeded":
            return row, record, None
        return row, record, {
            name: self.runs.table(identity, name) for name in ("equity", "settlements", "fills")
        }

    async def on_get_run(self, request, reply):
        row, record, tables = await self.read(self.run_detail, request.id)
        summary(reply.summary, row)
        reply.record = record
        if tables is None:
            return
        for point in tables["equity"].iter_rows(named=True):
            reply.equity.add(
                time_ms=point["time_ns"] // 1_000_000,
                equity=decimal(point["equity"]),
                settlement=point["event"] == "settlement",
            )
        for day in tables["settlements"].iter_rows(named=True):
            reply.days.add(
                trading_day=day["trading_day"].isoformat(),
                settled_ms=day["time_ns"] // 1_000_000,
                equity=decimal(day["equity"]),
                balance=decimal(day["balance"]),
                fees=decimal(day["fees"]),
                margin=decimal(day["margin"]),
            )
        for fill in tables["fills"].iter_rows(named=True):
            reply.fills.add(
                id=fill["fill"],
                order_id=fill["order"],
                time_ms=fill["time_ns"] // 1_000_000,
                trading_day=fill["trading_day"].isoformat(),
                contract=fill["contract"],
                side=Side.Value(f"SIDE_{fill['side'].upper()}"),
                offset=Offset.Value(f"OFFSET_{fill['offset'].upper()}"),
                lots=decimal(fill["quantity"]),
                price=decimal(fill["price"]),
                fee=decimal(fill["fee"]),
            )

    async def on_rename_run(self, request, _):
        await asyncio.to_thread(self.runs.rename, request.id, request.label)

    async def on_get_run_summary(self, request, result):
        row = await self.read(self.runs.get, request.id)
        if row is None:
            raise ValueError(f"no run {request.id}")
        summary(result, row)

    async def on_list_recent_runs(self, _, result):
        for row in await self.read(self.runs.recent):
            summary(result.runs.add(), row)

    async def on_list_runs(self, request, result):
        limit, order = request.limit or 50, request.order_by or "started"
        if not 1 <= limit <= 200:
            raise ValueError("run pages contain 1 to 200 records")
        if order not in ("started", "total_return", "annual_return", "sharpe", "max_drawdown", "calmar", "fills"):
            raise ValueError(f"unknown run sort: {order}")
        rows, result.has_more, strategies = await self.read(
            self.runs.page, limit, request.offset, request.strategy, request.search, order, request.ascending
        )
        result.strategies.extend(strategies)
        for row in rows:
            row["sweep_running"] = (
                row.get("sweep_running", False)
                or any(item[0] == row["sweep"] for item in self.waiting)
                or row["sweep"] in self.rolling
                or row["sweep"] in self.bayes
            )
            summary(result.runs.add(), row)
