"""Read-only recorder client; incomplete minutes and observation gaps survive import."""
import datetime
import socket
import struct
import uuid
from asterion.recorder_pb2 import Request, Response
from .base import Source, Batch, SourceError, decimal


class Recorder(Source):
    identity = "recorder"
    tables = ("minutes",)
    requires_endpoint = True

    def __init__(self, host, port, cancelled=lambda: False):
        super().__init__(cancelled)
        if not host or len(host) > 253 or any(c.isspace() for c in host) or not 1 <= port <= 65535:
            raise ValueError("recorder host and TCP port are required")
        self.host, self.port = host, port

    def exchange(self, operation, **values):
        if self.cancelled():
            raise SourceError("cancelled", "acquisition cancelled")
        request = Request(version=1, correlation_id=uuid.uuid4().hex)
        getattr(request, operation).SetInParent()
        for key, value in values.items():
            setattr(getattr(request, operation), key, value)
        payload = request.SerializeToString()
        self.requests += 1
        try:
            with socket.create_connection((self.host, self.port), timeout=10) as stream:
                stream.settimeout(10)
                # A plain TCP connection to an Asterion service starts with "ASTR".
                stream.sendall(b"ASTR" + struct.pack("!I", len(payload)) + payload)
                def read(size):
                    data = bytearray()
                    while len(data) < size:
                        if self.cancelled():
                            raise SourceError("cancelled", "acquisition cancelled")
                        chunk = stream.recv(size-len(data))
                        if not chunk:
                            raise SourceError("unavailable", "recorder closed the connection")
                        data.extend(chunk)
                    return bytes(data)
                size, = struct.unpack("!I", read(4))
                if not 0 < size <= 64 * 1024 * 1024:
                    raise SourceError("invalid_data", "invalid recorder response size")
                response = Response.FromString(read(size))
        except OSError:
            raise SourceError("unavailable", "recorder could not be reached") from None
        if response.version != 1 or response.correlation_id != request.correlation_id:
            raise SourceError("invalid_data", "recorder response identity mismatch")
        if response.WhichOneof("body") == "error":
            raise SourceError(response.error.code, response.error.message)
        if response.WhichOneof("body") != operation:
            raise SourceError("invalid_data", "recorder returned another operation")
        return getattr(response, operation)

    def coverage(self):
        return {"known": True, "days": [datetime.datetime.strptime(day.trading_day, "%Y%m%d").date().isoformat()
                                         for day in self.exchange("days").days],
                "note": "recorded days do not imply complete sessions or coverage of every contract"}

    def read(self, query):
        self.check(query)
        if not query.contract:
            raise ValueError("recorded minutes require a contract")
        exchange, product, month = query.contract.split("/")
        symbol = product + (month[3:4] + month[5:] if exchange == "CZCE" else month[2:4]+month[5:])
        if exchange in ("CZCE", "CFFEX"):
            symbol = symbol.upper()
        compact = query.day.replace("-", "")
        days = self.exchange("days")
        day = next((item for item in days.days if item.trading_day == compact), None)
        if day is None:
            return Batch([])
        result = self.exchange("minutes", trading_day=compact, venue=exchange, symbol=symbol)
        if (result.trading_day, result.venue, result.symbol) != (compact, exchange, symbol):
            raise SourceError("invalid_data", "recorder returned another partition")
        rows = [{"exchange": exchange, "product": product, "contract": query.contract,
                 "day": query.day, "start_ms": bar.start_ms, "partial": bar.partial,
                 **{key: decimal(getattr(bar, key)) for key in
                    ("open", "high", "low", "close", "volume", "average_price", "open_interest")}}
                for bar in result.bars]
        observations = [{"exchange": exchange, "contract": query.contract, "day": query.day,
                         "instance": segment.instance, "number": segment.number,
                         "first_received_ms": segment.first_received_ms,
                         "last_received_ms": segment.last_received_ms,
                         "ticks": segment.ticks, "end": segment.end}
                        for segment in day.segments]
        return Batch(rows, observations)
