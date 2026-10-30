"""Tushare's existing exact-decimal mappings behind the common source API."""
from asterion_lab.tushare import Tushare
from .base import Source, Batch


class TushareSource(Source):
    identity = "tushare"
    tables = ("daily", "calendar", "contracts")
    requires_credential = True

    def __init__(self, credential, budget, cancelled=lambda: False, **options):
        super().__init__(cancelled)
        self.client = Tushare(credential, budget, cancelled=lambda: self.cancelled(), **options)

    def read(self, query):
        self.check(query)
        try:
            if query.table == "calendar":
                rows = self.client.calendar(query.exchange, int(query.day[:4]))
            elif query.table == "contracts":
                rows = [item.row for item in self.client.contracts(query.exchange)]
            else:
                rows = self.client.daily(query.exchange, query.day)
            if query.contract:
                rows = [row for row in rows if row.get("contract") == query.contract]
            return Batch(rows)
        finally:
            self.requests = self.client.requests
