"""Terminal-owned offline analytics, using the Kernel Unix socket frame contract."""
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation
import math
import socket
import struct
import time
import sys
import warnings

import matplotlib
matplotlib.use("Agg")

import pandas as pd
import quantstats as qs
from asterion.v1.analytics_pb2 import QuantStatsRequest, QuantStatsReply, QuantStatsReport


def analyze(request: QuantStatsRequest) -> QuantStatsReport:
    if not 20 <= len(request.settlements) <= 200000:
        raise ValueError("QuantStats requires at least 20 settlement observations")
    previous = Decimal(request.deposit)
    if not previous.is_finite() or previous <= 0:
        raise ValueError("QuantStats requires positive finite equity")
    deposit = previous
    days, values, normalized_equity = [], [], [1.0]
    for row in request.settlements:
        day = date.fromisoformat(row.trading_day)
        if day.isoformat() != row.trading_day or (days and day <= days[-1]):
            raise ValueError("QuantStats settlement dates must ascend")
        equity = Decimal(row.equity)
        if not equity.is_finite() or equity <= 0:
            raise ValueError("QuantStats requires positive finite equity")
        values.append(float((equity - previous) / previous))
        normalized_equity.append(float(equity / deposit))
        days.append(day)
        previous = equity
    returns = pd.Series(values, index=pd.to_datetime(days), dtype=float)
    if not all(math.isfinite(value) for value in values + normalized_equity):
        raise ValueError("QuantStats requires finite daily returns")
    result = QuantStatsReport(
        task_id=request.task_id,
        result_sha256=request.result_sha256,
        library_version=qs.__version__,
        periods_per_year=252,
    )
    for day, value in zip(days, values, strict=True):
        result.returns.add(trading_day=day.isoformat(), value=value)
    # These are statistics of actual cost-inclusive settlement returns. No
    # symbol/benchmark is passed to the library; it never obtains market data.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # Undefined ratios stay absent.
        metrics = {
            "sortino": qs.stats.sortino(returns, rf=0, periods=252) if (returns < 0).any() else math.nan,
            # No losses means Omega is undefined. This also avoids the library's
            # price-detection heuristic for nonnegative returns above 100%.
            "omega": qs.stats.omega(returns, rf=0, required_return=0, periods=252)
                if (returns < 0).any() else math.nan,
            "tail_ratio": qs.stats.tail_ratio(returns, cutoff=0.95, prepare_returns=False),
            # Explicit normalized equity includes the initial capital baseline.
            # Its maximum is >= 1, so QuantStats treats it as prices.
            "ulcer_index": qs.stats.ulcer_index(pd.Series(normalized_equity,
                index=pd.to_datetime([days[0] - timedelta(days=1), *days]))),
            "value_at_risk": qs.stats.value_at_risk(returns, confidence=0.95, prepare_returns=False),
            "conditional_value_at_risk": qs.stats.conditional_value_at_risk(
                returns, confidence=0.95, prepare_returns=False),
        }
    for name, value in metrics.items():
        if math.isfinite(value):
            setattr(result, name, value)
    return result


def main():
    # Native owns the private directory and this one-request child. Match
    # kernel/ipc/local_channel.hpp: a 4-byte network-order length and Protobuf.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(sys.argv[1])
        listener.listen(1)
        listener.settimeout(30)
        connection, _ = listener.accept()
        with connection:
            deadline = time.monotonic() + 30
            def read(size):
                data = bytearray()
                while len(data) < size:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("QuantStats analysis timed out")
                    connection.settimeout(remaining)
                    chunk = connection.recv(size - len(data))
                    if not chunk:
                        raise EOFError("QuantStats IPC closed")
                    data.extend(chunk)
                return data
            size, = struct.unpack("!I", read(4))
            if not 0 < size <= 64 * 1024 * 1024:
                raise ValueError("invalid IPC frame length")
            request = QuantStatsRequest.FromString(read(size))
            reply = QuantStatsReply()
            try:
                reply.report.CopyFrom(analyze(request))
            except (ValueError, InvalidOperation) as error:
                reply.error.code = "invalid_request"
                reply.error.message = str(error)
            data = reply.SerializeToString()
            connection.settimeout(max(0, deadline - time.monotonic()))
            connection.sendall(struct.pack("!I", len(data)) + data)


if __name__ == "__main__":
    main()
