"""Asterion plugin protocol 1, using the terminal-supplied native wire binding."""


def serve(dispatch):
    """Handle bounded JSON-line calls until EOF. dispatch(method, params) returns JSON-compatible data."""
    from asterion_bindings.communication import activate
    from asterion_bindings.transport import serve as serve_transport

    def invoke(request):
        with activate(request["context"]):
            return dispatch(request["contract"], request["payload"])

    serve_transport(invoke)


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
