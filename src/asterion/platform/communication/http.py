"""HTTP binding: OpenAPI owns operations, shared context owns causality and deadlines."""

import json

from asterion_bindings.communication import activate, context, ingress
from starlette.responses import JSONResponse

HEADER = "x-asterion-context"


class CommunicationMiddleware:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        headers = dict(scope["headers"])
        raw = headers.get(HEADER.encode())
        try:
            # Plain HTTP is also a supported boundary: the trusted ingress creates
            # a root context when the caller has no parent trace. Never grants identity.
            value = ingress(raw, 60)
        except TimeoutError:
            response = JSONResponse(
                {"code": "DEADLINE_EXCEEDED", "detail": "请求已超过截止时间"}, status_code=408
            )
            return await response(scope, receive, send)
        except (ValueError, UnicodeError):
            response = JSONResponse(
                {"code": "INVALID_COMMUNICATION", "detail": "通信上下文不符合当前契约"},
                status_code=422,
            )
            return await response(scope, receive, send)
        scope.setdefault("state", {})["communication"] = value

        async def respond(message):
            if message["type"] == "http.response.start":
                message["headers"] = list(message.get("headers", [])) + [
                    (HEADER.encode(), json.dumps(value, separators=(",", ":")).encode())
                ]
            await send(message)

        with activate(value):
            await self.app(scope, receive, respond)


def outbound(request, parent=None):
    """httpx hook for the core worker transport; never used for vendor requests."""
    if parent is not None:
        with activate(parent):
            return outbound(request)
    request.headers[HEADER] = json.dumps(
        context(float(request.extensions.get("timeout", {}).get("read") or 30)),
        separators=(",", ":"),
    )
