"""Market-wide acquisition from Tushare into the warehouse.

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

from asterion_lab.tushare import EXCHANGES, SourceError, Tushare
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


class Acquisition:
    def __init__(self, warehouse: Warehouse, log):
        self.warehouse = warehouse
        self.log = log
        self.tasks = {}
        self.lock = threading.Lock()

    def running(self):
        return next((task for task in self.tasks.values() if task.state == "running"), None)

    def submit(self, identity: str, begin: str, end: str, client_factory) -> Task:
        first, last = datetime.date.fromisoformat(begin), datetime.date.fromisoformat(end)
        if first > last or first.year < 1990:
            raise ValueError("acquisition needs a range of days from 1990 on")
        with self.lock:
            if identity in self.tasks:
                raise ValueError(f"task {identity} already exists")
            if self.running():
                raise ValueError("another acquisition is running; wait for it or cancel it")
            task = Task(identity, begin, end)
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

    def acquire(self, task: Task, client: Tushare):
        today = datetime.date.today()
        first = datetime.date.fromisoformat(task.begin)
        last = min(datetime.date.fromisoformat(task.end), today)
        writer = self.warehouse.writer()

        def commit():
            task.version = writer.commit().identity

        for exchange in EXCHANGES:
            for year in range(first.year, last.year + 1):
                key = f"calendar/{exchange}/{year}"
                if not writer.base.has(key) or year == today.year:
                    writer.put_rows(key, "calendar", client.calendar(exchange, year))
                    task.requests = client.requests
            listings = client.contracts(exchange)
            writer.put_rows(f"contracts/{exchange}", "contracts",
                            [listing.row for listing in listings], as_of=today.isoformat())
            task.requests = client.requests
        commit()
        plan = []
        for exchange in EXCHANGES:
            open_days = set()
            for year in range(first.year, last.year + 1):
                calendar = self.warehouse.read(writer.base, f"calendar/{exchange}/{year}")
                open_days |= {day.isoformat() for day, is_open in
                              zip(calendar["day"], calendar["open"]) if is_open}
            stored = writer.base.days(exchange)
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
                rows = client.daily(exchange, day)
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
