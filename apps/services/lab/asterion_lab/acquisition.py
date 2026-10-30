"""Source-specific acquisition into an immutable warehouse.

One task at a time, on its own thread: calendars of the years asked (once per
exchange and year, again for the current year), today's contract lists, then
daily bars of every open day not yet stored. A version is committed after each
month of an exchange, so a failed or cancelled task keeps what it stored. A day
the vendor has no rows for yet stays missing and is asked again next time.
"""
from dataclasses import dataclass, field
import datetime
import threading
import time

from asterion_lab.sources.base import EXCHANGES, SourceError, Query
from asterion_lab.warehouse import Warehouse

STATES = ("running", "succeeded", "failed", "cancelled")


@dataclass
class Task:
    identity: str
    begin: str
    end: str
    state: str = "running"
    days_total: int = 0
    days_done: int = 0
    days_empty: int = 0
    requests: int = 0
    error_code: str = ""
    error: str = ""
    version: str = ""
    started_ns: int = field(default_factory=time.time_ns)
    finished_ns: int = 0
    cancel: threading.Event = field(default_factory=threading.Event)
    exchanges: tuple = EXCHANGES
    # Read the days again, though stored: a new version replaces their rows,
    # earlier versions keep theirs.
    refresh: bool = False
    provider: str = "tushare"
    table: str = "daily"
    contract: str = ""


class Acquisition:
    def __init__(self, warehouse: Warehouse, log):
        self.warehouse = warehouse
        self.log = log
        self.tasks = {}
        self.lock = threading.Lock()

    def running(self):
        return next((task for task in self.tasks.values() if task.state == "running"), None)

    def submit(self, identity: str, begin: str, end: str, client_factory, exchanges=(),
               refresh=False, provider="tushare", table="daily", contract="") -> Task:
        first, last = datetime.date.fromisoformat(begin), datetime.date.fromisoformat(end)
        if first > last or first.year < 1990:
            raise ValueError("acquisition needs a range of days from 1990 on")
        with self.lock:
            if identity in self.tasks:
                raise ValueError(f"task {identity} already exists")
            if self.running():
                raise ValueError("another acquisition is running; wait for it or cancel it")
            unknown = set(exchanges) - set(EXCHANGES)
            if unknown:
                raise ValueError(f"unknown exchanges {sorted(unknown)}")
            task = Task(identity, begin, end, exchanges=tuple(exchanges) or EXCHANGES,
                        refresh=refresh, provider=provider, table=table, contract=contract)
            self.tasks[identity] = task
        threading.Thread(target=self.run, args=(task, client_factory), daemon=True,
                         name=f"acquisition-{identity}").start()
        return task

    def cancel(self, identity: str):
        task = self.tasks.get(identity)
        if task is None:
            raise ValueError(f"no task {identity}")
        task.cancel.set()

    def run(self, task: Task, client_factory):
        self.log.write("info", "acquisition", "task.started", task=task.identity,
                       begin=task.begin, end=task.end)
        client = None
        try:
            client = client_factory(task.cancel.is_set)
            self.acquire(task, client)
            if task.cancel.is_set():
                raise SourceError("cancelled", "acquisition cancelled")
            task.state = "succeeded"
        except SourceError as error:
            task.state = "cancelled" if error.code == "cancelled" else "failed"
            task.error_code, task.error = error.code, str(error)
        except Exception as error:  # a broken row or file: reported, never retried
            task.state, task.error_code, task.error = "failed", "operation_failed", str(error)
        finally:
            if client is not None:
                task.requests = client.requests
            task.finished_ns = time.time_ns()
            self.log.write("info" if task.state == "succeeded" else "warning", "acquisition",
                           "task.finished", task=task.identity, state=task.state,
                           requests=task.requests, days=task.days_done, error=task.error)

    def acquire(self, task: Task, client):
        if task.table != "daily" or not {"calendar", "contracts"} <= set(client.tables):
            return self.acquire_partitions(task, client)
        today = datetime.date.today()
        first = datetime.date.fromisoformat(task.begin)
        last = min(datetime.date.fromisoformat(task.end), today)
        writer = self.warehouse.writer()

        def commit():
            task.version = writer.commit().identity

        for exchange in task.exchanges:
            for year in range(first.year, last.year + 1):
                key = f"calendar/{exchange}/{year}"
                if not writer.base.has(key) or year == today.year:
                    writer.put_rows(key, "calendar", client.read(Query("calendar", exchange, f"{year}-01-01")).rows)
                    task.requests = client.requests
            writer.put_rows(f"contracts/{exchange}", "contracts",
                            client.read(Query("contracts", exchange, task.begin)).rows,
                            as_of=today.isoformat())
            task.requests = client.requests
        commit()
        plan = []
        for exchange in task.exchanges:
            open_days = set()
            for year in range(first.year, last.year + 1):
                calendar = self.warehouse.read(writer.base, f"calendar/{exchange}/{year}")
                open_days |= {day.isoformat() for day, is_open in
                              zip(calendar["day"], calendar["open"]) if is_open}
            stored = set() if task.refresh else writer.base.days(exchange)
            plan += [(exchange, day) for day in sorted(open_days)
                     if first.isoformat() <= day <= last.isoformat() and day not in stored]
        task.days_total = len(plan)
        pending = {}  # (exchange, month) -> (rows, days)

        def flush(exchange, month):
            rows, days = pending.pop((exchange, month))
            if days:
                writer.add_days(exchange, month, rows, days)
                commit()

        try:
            for exchange, day in plan:
                month = day[:7]
                for key in [key for key in pending if key != (exchange, month)]:
                    flush(*key)
                rows = client.read(Query("daily", exchange, day)).rows
                task.requests = client.requests
                bucket = pending.setdefault((exchange, month), ([], []))
                if rows:
                    bucket[0].extend(rows)
                    bucket[1].append(day)
                else:
                    task.days_empty += 1
                task.days_done += 1
        finally:
            # Days already read are stored however the task ends.
            for key in list(pending):
                flush(*key)

    def acquire_partitions(self, task, client):
        first, last = map(datetime.date.fromisoformat, (task.begin, task.end))
        last = min(last, datetime.date.today())
        writer = self.warehouse.writer()
        # Futures trading days are weekdays; holidays are found by asking.
        days = [day.isoformat() for day in (first + datetime.timedelta(days=n)
                for n in range((last-first).days+1)) if day.weekday() < 5]
        task.days_total = len(days) * len(task.exchanges)
        for exchange in task.exchanges:
            for day in days:
                # A partial contract query must never claim exchange-wide coverage.
                suffix = "/" + task.contract.split("/", 1)[1] if task.contract else ""
                key = f"{task.table}/{exchange}/{day}{suffix}"
                if writer.base.has(key) and not task.refresh:
                    task.days_done += 1
                    continue
                batch = client.read(Query(task.table, exchange, day, task.contract))
                task.requests = client.requests
                if batch.rows:
                    writer.put_rows(key, task.table, batch.rows, days=[day])
                    if batch.observations:
                        writer.put_rows(f"observations/{exchange}/{day}{suffix}", "observations",
                                        batch.observations, days=[day])
                    task.version = writer.commit().identity
                else:
                    # An empty answer is not proof of a closed trading day.
                    task.days_empty += 1
                task.days_done += 1
