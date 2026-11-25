"""Requests of the lab service. One asyncio loop owns the connections; reads of
fixed Warehouse versions run on a small thread pool."""
import asyncio
import collections
from dataclasses import dataclass
import datetime
import hashlib
import itertools
import json
import math
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import os
import re
import shutil
from pathlib import Path
import tempfile

from asterion.common_pb2 import ErrorCode, Period, Side, Offset
from asterion.lab_pb2 import (AdmittedFactor, AdmittedRun, FactorResult, Request, Response,
                              RollingRun, RunState)
from asterion.strategy_pb2 import Bar, Warmup

from asterion_lab import backtest, chart, dominant, pit, repository, package, rolling, worker
from asterion_lab.runs import BATCH_STRATEGY, IDENTITY, SLIPPAGE_STRESS, Runs
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


def ctp_instrument(exchange: str, cased_product: str, month: str):
    from asterion.common_pb2 import Instrument
    suffix = month[3:4] + month[5:] if exchange == "CZCE" else month[2:4] + month[5:]
    return Instrument(exchange=exchange, symbol=cased_product + suffix)


def contract_name(selection) -> str:
    series_match = re.fullmatch(r"(SHFE|INE|DCE|CZCE|CFFEX|GFEX)/([a-z]{1,8})", selection.contract)
    if series_match:
        exchange, product = series_match.groups()
        prev = ""
        for entry in selection.candidates:
            datetime.date.fromisoformat(entry.delivery_month + "-01")
            suffix = entry.delivery_month[3:4] + entry.delivery_month[5:] if exchange == "CZCE" \
                else entry.delivery_month[2:4] + entry.delivery_month[5:]
            if entry.instrument.exchange != exchange \
                    or entry.instrument.symbol.lower() != product + suffix \
                    or (prev and entry.delivery_month <= prev):
                raise ValueError(f"invalid dominant warmup candidate for {selection.contract}")
            prev = entry.delivery_month
        return selection.contract
    instrument = selection.instrument
    match = re.fullmatch(r"(SHFE|INE|DCE|CZCE|CFFEX|GFEX)/([a-z]{1,8})/([0-9]{4}-[0-9]{2})",
                         selection.contract)
    if not match:
        raise ValueError(f"invalid historical contract {selection.contract}")
    exchange, product, month = match.groups()
    datetime.date.fromisoformat(month + "-01")
    suffix = month[3:4] + month[5:] if exchange == "CZCE" else month[2:4] + month[5:]
    if instrument.exchange != exchange or instrument.symbol.lower() != product + suffix:
        raise ValueError(f"historical contract does not match {instrument.exchange}/{instrument.symbol}")
    return selection.contract


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
    item.sweep_size = row.get("sweep_size", 0)
    item.sweep_succeeded = row.get("sweep_succeeded", 0)
    item.sweep_running = row.get("sweep_running", False)
    item.sweep_strategies = row.get("sweep_strategies", 0)
    # A sweep's run that has not started has no record yet.
    if "symbols" in row:
        item.label = row["label"]
        item.symbols.extend(row["symbols"])
        item.first_day, item.last_day = row["begin_day"], row["end_day"]
        item.period = next(key for key, value in PERIODS.items()
                           if value == ("day" if row["period"] == "day" else int(row["period"][:-1])))
        parameters(item.params, row["params"])
        run_settings(item.settings, row["settings"])
    return item


def run_settings(item, settings: dict):
    costs, risk, statistics = settings["costs"], settings["risk"], settings["statistics"]
    item.deposit.text = costs["deposit"]
    item.slippage_ticks = costs["slippage_ticks"]
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


def stored_span(found: dict, symbols, minutes: bool) -> tuple[str, str]:
    table = "minutes" if minutes else "daily"
    firsts, lasts = [], []
    for symbol in symbols:
        matched = [
            found[c][table]
            for c in found
            if table in found[c]
            and (c == symbol if symbol.count("/") == 2 else c.startswith(f"{symbol}/"))
        ]
        if not matched:
            raise ValueError(f"no stored {table} bars for {symbol}")
        firsts.append(min(item[0] for item in matched))
        lasts.append(max(item[1] for item in matched))
    overlap_first, overlap_last = max(firsts), min(lasts)
    if overlap_first <= overlap_last:
        return overlap_first, overlap_last
    return min(firsts), max(lasts)


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
        copies = [path for path in (self.directory / "sources").glob(f"{name}-*")
                  if path.name.rsplit("-", 1)[0] == name]
        if len(copies) != 1:
            raise ValueError(f"the frozen source of run {request.run_id} is missing or ambiguous")
        result = package.build(copies[0], row["strategy"])
        research = result.manifest.research
        research.run_id = request.run_id
        research.dataset_version = row["dataset_version"]
        research.symbols.extend(row["symbols"])
        research.period = next(key for key, value in PERIODS.items()
                               if value == ("day" if row["period"] == "day" else int(row["period"][:-1])))
        parameters(research.params, row["params"])
        research.weight_limits.gross.text = record["risk"]["max_gross_weight"]
        research.weight_limits.net.text = record["risk"]["max_net_weight"]
        if record["risk"].get("max_margin_ratio", "0") != "0":
            research.weight_limits.max_margin_ratio.text = record["risk"]["max_margin_ratio"]
        if record["risk"].get("min_rebalance_weight", "0") != "0":
            research.weight_limits.min_rebalance_weight.text = record["risk"]["min_rebalance_weight"]
        return result

    def build_warmup(self, request):
        if not 1 <= len(request.instruments) <= 64:
            raise ValueError("warmup requires 1 to 64 instruments")
        if not 1 <= request.max_bars <= 500:
            raise ValueError("warmup max_bars must be between 1 and 500")
        period = PERIODS.get(request.period)
        if period != "day" and period not in chart.MINUTES:
            raise ValueError("warmup bars are daily or of 1 to 60 minutes")
        contracts = [contract_name(selection) for selection in request.instruments]
        has_series = any(len(c.split("/")) == 2 for c in contracts)
        tables = ("daily",) if period == "day" else (("daily", "minutes") if has_series else ("minutes",))
        identity = self.data_version(request.dataset_version, tables)
        warehouse = self.dataset()
        with warehouse:
            version = warehouse.version(identity)
            grouped = collections.defaultdict(dict)
            daily_cache = {}
            delist_cache = {}
            pit_cache = {}
            lookback_days = max(request.max_bars * 2 + 120, 400)

            def exchange_pit(exchange_name):
                if exchange_name not in pit_cache:
                    ex_days = sorted(version.days(exchange_name))
                    if ex_days:
                        latest = datetime.date.fromisoformat(ex_days[-1])
                        begin_w = (latest - datetime.timedelta(days=lookback_days)).isoformat()
                        pit_cache[exchange_name] = pit.load_exchange_pit(
                            warehouse, version, exchange_name, begin_w, ex_days[-1]
                        )
                    else:
                        pit_cache[exchange_name] = pit.EMPTY_PIT
                return pit_cache[exchange_name]

            def pit_extra(exchange_name, product_name, bar_day, bar_end_ms):
                rv, ml, ms, inv = exchange_pit(exchange_name).lookup(
                    product_name, bar_day, bar_end_ms
                )
                out = {}
                for key, val in (
                    ("receipt_volume", rv),
                    ("member_long", ml),
                    ("member_short", ms),
                    ("inventory_total", inv),
                ):
                    if math.isfinite(val):
                        out[key] = decimal(dominant.rounded(Decimal(f"{val:.8f}")))
                return out
            for index, selection in enumerate(request.instruments):
                inst, contract = selection.instrument, contracts[index]
                parts = contract.split("/")
                if len(parts) == 2:
                    exchange, product = parts
                    if exchange not in delist_cache:
                        delist_map = {}
                        contracts_key = f"contracts/{exchange}"
                        if version.has(contracts_key):
                            table = warehouse.read(version, contracts_key)
                            for c_name, delist_date in table.select("contract", "delist_date").rows():
                                if delist_date is not None:
                                    delist_map[c_name] = delist_date.isoformat()
                        delist_cache[exchange] = delist_map
                    cased = product.upper() if exchange in ("CZCE", "CFFEX") else product
                    if selection.candidates:
                        sample = selection.candidates[0]
                        suf = 3 if exchange == "CZCE" else 4
                        cased = sample.instrument.symbol[:-suf]
                    candidate_map = {
                        f"{exchange}/{product}/{entry.delivery_month}": entry.instrument
                        for entry in selection.candidates
                    }
                    by_contract = collections.defaultdict(dict)
                    exchange_days = sorted(version.days(exchange))
                    begin_window = ""
                    if exchange_days:
                        latest_day = datetime.date.fromisoformat(exchange_days[-1])
                        begin_window = (latest_day - datetime.timedelta(days=lookback_days)).isoformat()
                    for key in version.keys("daily", exchange=exchange, begin=begin_window):
                        data = daily_cache.get((exchange, key))
                        if data is None:
                            data = warehouse.read(version, key)
                            daily_cache[(exchange, key)] = data
                        filtered = data.filter(data["product"] == product)
                        for c_name, day, open_, high, low, close, settle, vol, oi in filtered.sort("day").select(
                                "contract", "day", "open", "high", "low", "close",
                                "settlement", "volume", "open_interest").rows():
                            by_contract[c_name][day.isoformat()] = (
                                day, open_, high, low, close, settle, vol, oi)
                    months = []
                    for c_name in sorted(by_contract):
                        days_map = by_contract[c_name]
                        traded_days = [
                            d for d, r in sorted(days_map.items())
                            if None not in (r[1], r[2], r[3], r[4]) and r[5] is not None
                        ]
                        if not traded_days:
                            continue
                        item = dominant.Month(
                            c_name,
                            c_name.rsplit("/", 1)[1],
                            last_bar=traded_days[-1],
                            delist_date=delist_cache[exchange].get(c_name, ""),
                        )
                        for d, r in days_map.items():
                            if r[5] is not None:
                                item.settlement[d] = Decimal(r[5])
                            if r[7] is not None:
                                item.open_interest[d] = Decimal(r[7])
                        months.append(item)
                    if len(months) < 2:
                        raise ValueError(f"dominant warmup for {contract} requires at least two months")
                    dom_plan = dominant.plan(months, traded=True)
                    day_segment = {}
                    for seg_idx, seg in enumerate(dom_plan.segments):
                        end_bound = dom_plan.segments[seg_idx + 1].begin \
                            if seg_idx + 1 < len(dom_plan.segments) else ""
                        m_contract = months[seg.month].contract
                        for d in dom_plan.days:
                            if d >= seg.begin and (not end_bound or d < end_bound):
                                day_segment[d] = (seg_idx, m_contract)
                    bars = []
                    emitted_rolls = set()
                    if period == "day":
                        prev_daily_close = {}
                        for c_name, days_map in by_contract.items():
                            prev_map, last_c = {}, None
                            for d_key, r in sorted(days_map.items()):
                                if None not in (r[1], r[2], r[3], r[4]):
                                    c_val = Decimal(r[4])
                                    if last_c is not None and last_c > 0:
                                        prev_map[d_key] = last_c
                                    last_c = c_val
                            prev_daily_close[c_name] = prev_map
                        for d in sorted(day_segment):
                            seg_idx, m_contract = day_segment[d]
                            row = by_contract[m_contract].get(d)
                            if row is None or None in (row[1], row[2], row[3], row[4]):
                                continue
                            day_obj, open_, high, low, close, _, vol, oi = row
                            m_inst = candidate_map.get(m_contract) or ctp_instrument(
                                exchange, cased, m_contract.rsplit("/", 1)[1])
                            end_ms = int(datetime.datetime.combine(
                                day_obj, datetime.time(15), chart.BEIJING).timestamp()) * 1000
                            extra = pit_extra(exchange, product, d, end_ms)
                            if oi is not None:
                                extra["open_interest"] = decimal(oi)
                            if seg_idx > 0 and seg_idx not in emitted_rolls:
                                extra["roll_ratio"] = decimal(dom_plan.ratios[seg_idx])
                                emitted_rolls.add(seg_idx)
                            near_px = Decimal(close)
                            prev_near = prev_daily_close.get(m_contract, {}).get(d)
                            if prev_near is not None and near_px > 0:
                                nr = dominant.simple_return(near_px, prev_near)
                                if nr is not None:
                                    extra["near_return"] = decimal(nr)
                            sub_idx = dom_plan.second.get(d)
                            if sub_idx is not None and near_px > 0:
                                sub_contract = months[sub_idx].contract
                                sub_row = by_contract[sub_contract].get(d)
                                if (sub_row is not None
                                        and None not in (sub_row[1], sub_row[2], sub_row[3], sub_row[4])
                                        and Decimal(sub_row[4]) > 0):
                                    sub_px = Decimal(sub_row[4])
                                    extra["far_close"] = decimal(sub_px)
                                    extra["roll_yield"] = decimal(dominant.annualized_roll_yield(
                                        near_px, sub_px,
                                        m_contract.rsplit("/", 1)[1], months[sub_idx].delivery_month))
                                    prev_sub = prev_daily_close.get(sub_contract, {}).get(d)
                                    if prev_sub is not None:
                                        fr = dominant.simple_return(sub_px, prev_sub)
                                        if fr is not None:
                                            extra["far_return"] = decimal(fr)
                            bars.append(Bar(
                                instrument=m_inst, trading_day=d, end_ms=end_ms,
                                open=decimal(open_), high=decimal(high), low=decimal(low),
                                close=decimal(close), volume=int(vol or 0), **extra))
                    else:
                        recent = []
                        for d in sorted(day_segment, reverse=True):
                            seg_idx, m_contract = day_segment[d]
                            _, prod, month = m_contract.split("/")
                            key = f"minutes/{exchange}/{d}/{prod}/{month}"
                            if key not in version.manifest["partitions"]:
                                continue
                            rows = warehouse.read(version, key).sort("start_ms").select(
                                "day", "start_ms", "partial", "open", "high", "low", "close",
                                "volume", "open_interest").rows()
                            day_bars = chart.minute_bars(rows, period, m_contract)
                            sub_closes = {}
                            sub_idx = dom_plan.second.get(d)
                            sub_contract = months[sub_idx].contract if sub_idx is not None else ""
                            sub_month = months[sub_idx].delivery_month if sub_idx is not None else ""
                            if sub_idx is not None:
                                _, s_prod, s_month = sub_contract.split("/")
                                s_key = f"minutes/{exchange}/{d}/{s_prod}/{s_month}"
                                if version.has(s_key):
                                    s_rows = warehouse.read(version, s_key).sort("start_ms").select(
                                        "day", "start_ms", "partial", "open", "high", "low", "close",
                                        "volume", "open_interest").rows()
                                    sub_closes = {
                                        sb["time_ms"]: Decimal(sb["close"])
                                        for sb in chart.minute_bars(s_rows, period, sub_contract)
                                        if not sb["partial"] and sb["close"] is not None
                                    }
                            recent.extend(
                                (seg_idx, m_contract, bar, sub_closes.get(bar["time_ms"]),
                                 sub_contract, sub_month)
                                for bar in reversed(day_bars) if not bar["partial"]
                            )
                            if len(recent) >= request.max_bars:
                                break
                        prev_min_close = {}
                        for seg_idx, m_contract, bar, sub_close, sub_contract, sub_month in reversed(
                            recent[:request.max_bars]
                        ):
                            m_inst = candidate_map.get(m_contract) or ctp_instrument(
                                exchange, cased, m_contract.rsplit("/", 1)[1])
                            t_day = bar["trading_day"].isoformat()
                            extra = pit_extra(exchange, product, t_day, bar["time_ms"])
                            if bar["open_interest"] is not None:
                                extra["open_interest"] = decimal(bar["open_interest"])
                            if seg_idx > 0 and seg_idx not in emitted_rolls:
                                extra["roll_ratio"] = decimal(dom_plan.ratios[seg_idx])
                                emitted_rolls.add(seg_idx)
                            near_px = Decimal(bar["close"])
                            prev_near = prev_min_close.get(m_contract)
                            if prev_near is not None and near_px > 0:
                                nr = dominant.simple_return(near_px, prev_near)
                                if nr is not None:
                                    extra["near_return"] = decimal(nr)
                            if near_px > 0:
                                prev_min_close[m_contract] = near_px
                            if sub_close is not None and sub_close > 0 and near_px > 0:
                                extra["far_close"] = decimal(sub_close)
                                extra["roll_yield"] = decimal(dominant.annualized_roll_yield(
                                    near_px, sub_close,
                                    m_contract.rsplit("/", 1)[1], sub_month))
                                prev_sub = prev_min_close.get(sub_contract)
                                if prev_sub is not None:
                                    fr = dominant.simple_return(sub_close, prev_sub)
                                    if fr is not None:
                                        extra["far_return"] = decimal(fr)
                                prev_min_close[sub_contract] = sub_close
                            bars.append(Bar(
                                instrument=m_inst, trading_day=t_day,
                                end_ms=bar["time_ms"], open=decimal(bar["open"]),
                                high=decimal(bar["high"]), low=decimal(bar["low"]),
                                close=decimal(bar["close"]), volume=int(bar["volume"]), **extra))
                    for bar in bars[-request.max_bars:]:
                        grouped[bar.end_ms][index] = bar
                    continue
                bars = []
                fixed_exchange, fixed_product = parts[0], parts[1]
                table = "daily" if period == "day" else "minutes"
                keys = list(version.keys(table, exchange=inst.exchange, contract=contract))
                for key in reversed(keys):
                    data = warehouse.read(version, key)
                    data = data.filter(data["contract"] == contract)
                    columns = ["open", "high", "low", "close", "volume", "open_interest"]
                    recent = []
                    if period == "day":
                        rows = data.sort("day").select("day", *columns).rows()
                        for day, open_, high, low, close, volume, interest in rows:
                            if None in (open_, high, low, close):
                                continue
                            end_ms = int(datetime.datetime.combine(
                                day, datetime.time(15), chart.BEIJING).timestamp()) * 1000
                            extra = pit_extra(fixed_exchange, fixed_product, day.isoformat(), end_ms)
                            if interest is not None:
                                extra["open_interest"] = decimal(interest)
                            recent.append(Bar(
                                instrument=inst, trading_day=day.isoformat(), end_ms=end_ms,
                                open=decimal(open_), high=decimal(high), low=decimal(low),
                                close=decimal(close), volume=int(volume or 0), **extra))
                    else:
                        rows = data.sort("start_ms").select("day", "start_ms", "partial", *columns).rows()
                        for bar in chart.minute_bars(rows, period, contract):
                            if bar["partial"]:
                                continue
                            t_day = bar["trading_day"].isoformat()
                            extra = pit_extra(fixed_exchange, fixed_product, t_day, bar["time_ms"])
                            if bar["open_interest"] is not None:
                                extra["open_interest"] = decimal(bar["open_interest"])
                            recent.append(Bar(
                                instrument=inst, trading_day=t_day,
                                end_ms=bar["time_ms"], open=decimal(bar["open"]),
                                high=decimal(bar["high"]), low=decimal(bar["low"]),
                                close=decimal(bar["close"]), volume=int(bar["volume"]), **extra))
                    bars = recent + bars
                    if len(bars) >= request.max_bars:
                        break
                for bar in bars[-request.max_bars:]:
                    grouped[bar.end_ms][index] = bar
        if not grouped:
            raise ValueError("no stored bars for the requested warmup instruments")
        warmup = Warmup(dataset_version=version.identity)
        digest = hashlib.sha256()
        for sequence, end_ms in enumerate(sorted(grouped)[-request.max_bars:], 1):
            batch = warmup.bars.add(sequence=sequence)
            digest.update(b"batch\n")
            for index in sorted(grouped[end_ms]):
                bar = batch.bars.add()
                bar.CopyFrom(grouped[end_ms][index])
                suffix = f"|{bar.roll_ratio.text}" if bar.HasField("roll_ratio") else ""
                for field, tag in (
                    ("roll_yield", "ry"),
                    ("far_close", "fc"),
                    ("near_return", "nr"),
                    ("far_return", "fr"),
                    ("receipt_volume", "rv"),
                    ("member_long", "ml"),
                    ("member_short", "ms"),
                    ("inventory_total", "inv"),
                ):
                    if bar.HasField(field):
                        suffix += f"|{tag}={getattr(bar, field).text}"
                line = (f"{bar.trading_day}|{bar.end_ms}|"
                        f"{bar.instrument.exchange}/{bar.instrument.symbol}|"
                        f"{bar.open.text}|{bar.high.text}|{bar.low.text}|{bar.close.text}|"
                        f"{bar.volume}|{bar.open_interest.text}{suffix}\n")
                digest.update(line.encode())
        warmup.sha256 = digest.hexdigest()
        return warmup

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
        admitted = AdmittedFactor(evaluation=request)
        admitted.pinned.dataset_version = self.data_version("", ("daily",))
        if request.symbols and (not request.first_day or not request.last_day):
            exchanges = {s.split("/")[0] for s in request.symbols if s}
            _, found, _ = self.stored_contracts(
                pinned=admitted.pinned.dataset_version,
                exchanges=exchanges,
                include_daily=True,
                include_names=False,
            )
            first, last = stored_span(found, request.symbols, False)
            if not admitted.evaluation.first_day:
                admitted.evaluation.first_day = first
            if not admitted.evaluation.last_day:
                admitted.evaluation.last_day = last
        admitted.source, revision = self.copy_source(
            request.repository, f"factor-{request.id}",
            f"factors/{request.factor.partition('.')[0]}.py")
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
            first, last = stored_span(found, request.symbols, is_minutes)
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
            version = warehouse.version(request.dataset_version) if request.dataset_version else warehouse.current()
            if period in chart.CALENDAR:
                rows = warehouse.query(version, """
                    select day, open, high, low, close, volume from daily
                    where contract = ? order by day""", ("daily",), (request.contract,),
                    exchange=exchange)
                return version, chart.calendar_bars(rows, period)
            rows = warehouse.query(version, """
                select day, start_ms, partial, open, high, low, close, volume from minutes
                where contract = ? order by start_ms""", ("minutes",), (request.contract,),
                contract=request.contract)
        return version, chart.minute_bars(rows, period, request.contract)

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
                name_exchanges = [only.split("/")[0]] if only else (sorted(exchanges) if exchanges else [""])
                for exchange in name_exchanges:
                    names.update(warehouse.query(version, "select contract, name from contracts",
                                                 ("contracts",), exchange=exchange))
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
        try:
            try:
                output = await worker.output(
                    "asterion_lab.factor", str(self.directory), cwd=admitted.source,
                    input=admitted.SerializeToString(), timeout=FACTOR_SECONDS)
            except TimeoutError:
                raise ValueError(f"the factor took longer than {FACTOR_SECONDS} s; "
                                 "evaluate fewer symbols or a shorter range")
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
            output = await worker.output("asterion_lab.report", str(self.directory), request.id,
                                         timeout=REPORT_SECONDS)
        except TimeoutError:
            raise ValueError(f"QuantStats took longer than {REPORT_SECONDS} s")
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

    def copy_source(self, root, name, entry):
        """A copy of the code `entry` needs for one admission alone, and the
        repository's revision."""
        (self.directory / "sources").mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=f"{name}-", dir=self.directory / "sources"))
        return str(destination), repository.snapshot(Path(root), destination, entry)

    def with_source(self, admitted, name, others=()):
        """The run with its strategy's code, and that of `others`, copied for it alone."""
        module = admitted.backtest.strategy.partition(".")[0]
        admitted.source, taken = self.copy_source(admitted.backtest.repository, name,
                                                  f"strategies/{module}.py")
        for other in others:
            repository.snapshot(Path(admitted.backtest.repository), Path(admitted.source),
                                f"strategies/{other.partition('.')[0]}.py")
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
                *worker.command(module), str(self.directory), cwd=source or request.source,
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
        while self.waiting and len(self.backtests) < self.waiting[0][2]:
            _, run, _ = self.waiting[0]
            try:
                await self.launch(run)
            except Exception as error:
                self.log.write("error", MODULE, "backtest.not_started", run=run.backtest.id,
                               message=str(error))
            self.waiting.popleft()
        for sweep, pending in list(self.rolling.items()):
            members = {f"{sweep}-{n}" for n in range(pending.candidates)}
            if pending.starting or any(item[0] == sweep for item in self.waiting) \
                    or members & set(self.backtests):
                continue
            # Kept until its process runs, so the sweep never looks finished before.
            pending.starting = True
            try:
                states = {row["state"] for row in await self.read(self.runs.sweep, sweep)}
                if states != {"succeeded"}:
                    self.log.write("warning", MODULE, "rolling.skipped", sweep=sweep,
                                   message="not every candidate succeeded")
                else:
                    await self.launch(pending.request, "asterion_lab.rolling",
                                      pending.request.base.source)
            except Exception as error:
                self.log.write("error", MODULE, "backtest.not_started", run=pending.request.id,
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
        symbols = list(request.symbols)
        if len(set(symbols)) != len(symbols) or any(not item.strip() for item in symbols):
            raise ValueError("a sweep lists distinct non-empty symbols")
        names = [item.name for item in request.values]
        if len(set(names)) != len(names) or any(not item.values for item in request.values):
            raise ValueError("a sweep gives each of its parameters one or more values")
        batch = list(request.strategies)
        slippages = list(request.slippage_ticks)
        if batch and (names or symbols or slippages or request.HasField("walk_forward")):
            raise ValueError("a strategy batch varies only the strategy")
        if batch and (len(batch) < 2 or len({item.strategy for item in batch}) != len(batch)):
            raise ValueError("a strategy batch runs two or more distinct strategies")
        if slippages and (names or symbols or batch or request.HasField("walk_forward")):
            raise ValueError("a slippage stress sweep varies only slippage_ticks")
        if slippages and (len(slippages) < 2 or len(set(slippages)) != len(slippages)):
            raise ValueError("a slippage stress sweep runs two or more distinct tick levels")
        if not batch and not slippages and not names and len(symbols) < 2:
            raise ValueError("a sweep varies parameters, slippage or two or more symbols")
        symbol_items = symbols or [None]
        slippage_items = slippages or [None]
        count = len(symbol_items) * len(slippage_items) * math.prod(len(item.values) for item in request.values)
        if count > 10000:
            raise ValueError(f"a sweep runs at most 10000 combinations, not {count}")
        combinations = (
            list(itertools.product(*[list(item.values) for item in request.values]))
            if names
            else [()]
        )
        if request.HasField("walk_forward") and len(symbols) > 1:
            raise ValueError("rolling validation compares parameter values on the same symbols")
        backtest.config_of(AdmittedRun(backtest=request.base))
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
        base = await asyncio.to_thread(self.with_source, base, f"sweep-{request.id}",
                                       [item.strategy for item in batch])
        runs, planned = [], []
        for symbol in symbol_items:
            span = (
                stored_span(found, [symbol], is_minutes)
                if symbol and found is not None
                else None
            )
            for values, choice, slip in itertools.product(combinations, batch or [None], slippage_items):
                n = len(runs)
                run = AdmittedRun()
                run.CopyFrom(base)
                run.backtest.id = f"{request.id}-{n}"
                if symbol is not None:
                    del run.backtest.symbols[:]
                    run.backtest.symbols.append(symbol)
                    if span is not None:
                        if not run.backtest.first_day:
                            run.backtest.first_day = span[0]
                        if not run.backtest.last_day:
                            run.backtest.last_day = span[1]
                for name, value in zip(names, values):
                    run.backtest.params[name].CopyFrom(value)
                swept = {name: getattr(value, value.WhichOneof("value"))
                         for name, value in zip(names, values)}
                if choice:
                    run.backtest.strategy = choice.strategy
                    run.backtest.params.clear()
                    for name, value in choice.params.items():
                        run.backtest.params[name].CopyFrom(value)
                    swept[BATCH_STRATEGY] = choice.strategy
                if slip is not None:
                    run.backtest.slippage_ticks = slip
                    swept[SLIPPAGE_STRESS] = slip
                runs.append(run)
                planned.append((run.backtest.id, swept))
        await self.checked_run(runs[0].backtest)
        for run in runs[1:]:
            self.check_run_id(run.backtest.id)
            backtest.config_of(run)
        rolling = None
        if request.HasField("walk_forward"):
            if request.base.holdout_day or len(runs) < 2:
                raise ValueError("rolling validation compares two or more candidates and takes no holdout")
            if not request.base.periods_per_year or not request.base.zero_risk_free_rate:
                raise ValueError("rolling validation requires periods_per_year and zero_risk_free_rate")
            if request.walk_forward.training_days < 20 or request.walk_forward.validation_days < 1:
                raise ValueError("rolling validation needs at least 20 training days and one validation day")
            rolling = RollingRun(id=f"{request.id}-rolling", sweep_id=request.id, base=base,
                                 walk_forward=request.walk_forward)
            self.check_run_id(rolling.id)
        await asyncio.to_thread(self.runs.plan_sweep, request.id, planned)
        limit = min(request.processes, os.cpu_count() or 1) if request.processes else self.processes
        if rolling:
            self.rolling[request.id] = Rolling(rolling, len(runs))
        self.waiting.extend((request.id, run, limit) for run in runs)
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
        rolling = await self.read(self.runs.get, f"{request.id}-rolling")
        if rolling:
            summary(reply.rolling, rolling)
        members = {row["id"] for row in rows} | {f"{request.id}-rolling"}
        reply.active = bool(waiting or members & set(self.backtests) or request.id in self.rolling)

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
        kept = {f"run-{identity}" for identity in self.runs.ids()}
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
            row = await self.read(self.runs.get, request.id)
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
        row = self.runs.get(identity)
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
            self.runs.page, limit, request.offset, request.strategy, request.search, order, request.ascending)
        result.strategies.extend(strategies)
        for row in rows:
            row["sweep_running"] = row.get("sweep_running", False) or any(
                item[0] == row["sweep"] for item in self.waiting) or row["sweep"] in self.rolling
            summary(result.runs.add(), row)
