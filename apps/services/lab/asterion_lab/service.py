"""Requests of the lab service. One asyncio loop owns the connections; reads of
the warehouse run on a small thread pool and acquisition on its own thread."""
import asyncio
import datetime
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import os
from pathlib import Path
import time

from asterion.lab_pb2 import Request, Response

from asterion_lab.acquisition import Acquisition
from asterion_lab import crosscheck, dominant
from asterion_lab.sessions import Sessions, holiday_between
from asterion_lab.tushare import Budget, SourceError, Tushare
from asterion_lab.version import VERSION
from asterion_lab.warehouse import Warehouse

MODULE = "lab-service"


def text(value) -> str:
    """An exact decimal without trailing zeros; empty when absent."""
    if value is None:
        return ""
    return format(Decimal(value).normalize(), "f")


class Service:
    def __init__(self, name: str, directory: Path, log):
        self.name = name
        self.log = log
        self.started = time.monotonic()
        self.initialized = False
        self.warehouse = Warehouse(directory)
        self.acquisition = Acquisition(self.warehouse, log)
        self.sources = {}  # provider -> (credential, Budget), in memory only
        self.reads = ThreadPoolExecutor(max_workers=2, thread_name_prefix="warehouse-read")
        self.sessions = Sessions()

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

    async def on_configure_source(self, request, response):
        if request.provider != "tushare":
            raise ValueError(f"unknown data source {request.provider}")
        budget = Budget(request.requests_per_minute)
        Tushare(request.credential, budget)  # validates the token's form
        self.sources["tushare"] = (request.credential, budget)
        self.log.write("info", MODULE, "source.configured", provider=request.provider,
                       requests_per_minute=request.requests_per_minute)
        response.accepted.SetInParent()

    async def on_acquire(self, request, response):
        if "tushare" not in self.sources:
            raise ValueError("enter or save the Tushare credential first")
        credential, budget = self.sources["tushare"]
        self.acquisition.submit(
            request.id, request.begin_day, request.end_day,
            lambda cancelled: Tushare(credential, budget, cancelled=cancelled),
            exchanges=list(request.exchanges), refresh=request.refresh)
        response.accepted.SetInParent()

    async def on_cancel(self, request, response):
        self.acquisition.cancel(request.id)
        response.accepted.SetInParent()

    async def on_tasks(self, _, response):
        for task in self.acquisition.tasks.values():
            response.tasks.tasks.add(
                id=task.identity, begin_day=task.begin, end_day=task.end, state=task.state,
                days_total=task.days_total, days_done=task.days_done, days_empty=task.days_empty,
                requests=task.requests, error_code=task.error_code, error=task.error,
                dataset_version=task.version, started_ns=task.started_ns,
                finished_ns=task.finished_ns)
        response.tasks.SetInParent()

    def summary(self):
        version = self.warehouse.current()
        tables = []
        for table in ("contracts", "calendar", "daily"):
            day = "list_date" if table == "contracts" else "day"
            (rows, first, last), = self.warehouse.query(
                version, f"select count(*), min({day}), max({day}) from {table}", (table,))
            partitions = sum(key.split("/")[0] == table for key in version.manifest["partitions"])
            tables.append((table, rows, partitions, first, last))
        return version, tables

    async def on_summary(self, _, response):
        version, tables = await self.read(self.summary)
        summary = response.summary
        summary.dataset_version = version.identity
        summary.created_ns = version.manifest["created_ns"]
        for table, rows, partitions, first, last in tables:
            summary.tables.add(table=table, rows=rows, partitions=partitions,
                               first_day=first.isoformat() if first else "",
                               last_day=last.isoformat() if last else "")

    def coverage(self):
        version = self.warehouse.current()
        rows = self.warehouse.query(version, """
            with d as (select exchange, product, strftime(day, '%Y-%m') as month,
                              count(distinct day) as days from daily group by all),
                 o as (select exchange, strftime(day, '%Y-%m') as month, count(*) as open_days
                       from calendar where open group by all)
            select d.exchange, d.product, d.month, d.days, coalesce(o.open_days, 0)
            from d left join o using (exchange, month) order by 1, 2, 3""", ("daily", "calendar"))
        return version, rows

    async def on_coverage(self, _, response):
        version, rows = await self.read(self.coverage)
        coverage = response.coverage
        coverage.dataset_version = version.identity
        current = None
        for exchange, product, month, days, open_days in rows:
            if current is None or (current.exchange, current.product) != (exchange, product):
                current = coverage.products.add(exchange=exchange, product=product)
            current.months.add(month=month, days=days, open_days=open_days)

    def daily_bars(self, contract, begin, end):
        version = self.warehouse.current()
        rows = self.warehouse.query(version, """
            select day, open, high, low, close, settlement, volume, open_interest from daily
            where contract = ? and day between ?::date and ?::date order by day""",
            ("daily",), (contract, begin, end), exchange=contract.split("/")[0])
        return version, rows

    async def on_daily_bars(self, request, response):
        version, rows = await self.read(self.daily_bars, request.contract, request.begin_day,
                                        request.end_day)
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
        version = self.warehouse.current()
        # The first day's dominant month is read from the day before it.
        rows = self.warehouse.query(version, """
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
        version = self.warehouse.current()
        rows = self.warehouse.query(version, """
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
                    second = crosscheck.akshare_daily(exchange, day)
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
