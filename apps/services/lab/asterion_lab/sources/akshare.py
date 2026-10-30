"""AKShare exchange daily bars, normalized without borrowing another source."""
from .base import Source, Batch, SourceError, contract_of, decimal


def fetch(exchange, day):
    import akshare
    compact = day.replace("-", "")
    return akshare.get_futures_daily(start_date=compact, end_date=compact, market=exchange)


class Akshare(Source):
    identity = "akshare"
    tables = ("daily",)

    def __init__(self, cancelled=lambda: False, fetcher=fetch):
        super().__init__(cancelled)
        self.fetcher = fetcher

    def read(self, query):
        self.check(query)
        self.requests += 1
        try:
            frame = self.fetcher(query.exchange, query.day)
            records = frame.to_dict("records")
        except Exception:
            raise SourceError("unavailable", "AKShare could not read the exchange daily file") from None
        self.check(query)
        rows = []
        seen = set()
        for record in records:
            try:
                date = str(record["date"]).replace("-", "")
                if date != query.day.replace("-", ""):
                    raise ValueError("unexpected trading day")
                contract = contract_of(query.exchange, str(record["symbol"]), query.day)
                if contract is None or (query.contract and query.contract != contract):
                    continue
                if contract in seen:
                    raise ValueError("duplicate contract")
                seen.add(contract)
                prices = {key: decimal(record.get(key)) for key in ("open", "high", "low", "close")}
                prices = {key: None if value == 0 else value for key, value in prices.items()}
                if all(prices[key] is None for key in ("open", "high", "low")):
                    prices["close"] = None
                rows.append({"exchange": query.exchange, "product": contract.split("/")[1],
                             "contract": contract, "day": query.day, **prices,
                             "settlement": decimal(record.get("settle")),
                             "previous_settlement": decimal(record.get("pre_settle")),
                             "volume": decimal(record.get("volume")),
                             "open_interest": decimal(record.get("open_interest")),
                             # Turnover units differ across exchange adapters. Do not invent
                             # a shared yuan amount until each mapping has been verified.
                             "amount": None, "previous_close": None,
                             "open_interest_change": None, "delivery_settlement": None})
            except (KeyError, ValueError, TypeError):
                raise SourceError("invalid_data", "AKShare returned an invalid daily partition") from None
        return Batch(rows)
