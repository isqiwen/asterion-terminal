"""Asterion plugin protocol 1. Standard library only; no terminal internals required."""

import json
import sys

PROTOCOL = 1


def serve(dispatch):
    """Handle bounded JSON-line calls until EOF. dispatch(method, params) returns JSON-compatible data."""
    while raw := sys.stdin.buffer.readline(8_000_001):
        if len(raw) > 8_000_000:
            raise ValueError("Plugin request too large")
        request = json.loads(raw)
        if (
            not isinstance(request, dict)
            or set(request) != {"protocol", "id", "method", "params"}
            or request["protocol"] != PROTOCOL
            or not isinstance(request["method"], str)
            or not isinstance(request["params"], dict)
        ):
            raise ValueError("Invalid plugin request")
        try:
            result = dispatch(request["method"], request["params"])
            response = {"protocol": PROTOCOL, "id": request["id"], "result": result}
        except Exception:  # noqa: BLE001 - never expose arbitrary plugin exceptions
            # Provider exceptions can contain credentials and upstream bodies.
            response = {
                "protocol": PROTOCOL,
                "id": request["id"],
                "error": "Plugin operation failed",
            }
        sys.stdout.write(json.dumps(response, ensure_ascii=False, allow_nan=False) + "\n")
        sys.stdout.flush()


def serve_strategy(create, warmup):
    """Stream immutable closed bars to one fresh session; parameters are explicit."""
    from datetime import date
    from decimal import Decimal, localcontext

    session = None
    previous = None

    def dispatch(method, params):
        nonlocal session, previous
        if method == "strategy.prepare" and set(params) == {"parameters"}:
            return warmup(params["parameters"])
        if method == "strategy.open" and set(params) == {"parameters"} and session is None:
            with localcontext() as context:
                context.prec = 40
                session = create(params["parameters"])
            return None
        if (
            method == "strategy.close"
            and set(params) == {"trading_day", "close"}
            and session is not None
        ):
            day = date.fromisoformat(params["trading_day"])
            close = Decimal(params["close"])
            if previous is not None and day <= previous or not close.is_finite() or close <= 0:
                raise ValueError("Invalid closed bar")
            previous = day
            with localcontext() as context:
                context.prec = 40
                result = session.on_close(day, close)
            if type(result) is not bool:
                raise ValueError("Strategy must return bool")
            return result
        raise ValueError("Unsupported strategy operation")

    serve(dispatch)
