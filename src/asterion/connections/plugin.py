"""Connection UI/API ownership; connector discovery uses existing plugin hooks."""

from contextlib import asynccontextmanager

from fastapi import APIRouter, Depends, HTTPException

from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.plugins import Activation, Plugin
from asterion.platform.resources import DATA_ROOT

from .public import CONNECTIONS, CONNECTOR_OWNERS, CREDENTIALS, ConnectionAccess
from .service import (
    ConnectionList,
    ConnectionService,
    ConnectionView,
    DeleteConnection,
    SaveConnection,
)


def activate(context):
    service = ConnectionService(
        context.resource(DATA_ROOT),
        context.resource(CREDENTIALS),
        context.resource(CONNECTOR_OWNERS),
    )

    @asynccontextmanager
    async def lifespan(app):
        service.initialize(context.hooks("connections.connectors"))
        try:
            yield
        finally:
            service.close()

    router = APIRouter(
        prefix="/api/v1/connections",
        dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)],
        lifespan=lifespan,
    )

    def call(function, *args):
        try:
            return function(*args)
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    @router.get("", response_model=ConnectionList)
    def state():
        return service.snapshot()

    @router.post("", response_model=ConnectionView)
    def save(body: SaveConnection):
        return call(service.save, body)

    @router.post("/{connection_id}/connect", response_model=ConnectionView)
    def connect(connection_id: str):
        return call(service.connect, connection_id)

    @router.post("/{connection_id}/select", response_model=ConnectionView)
    def select(connection_id: str):
        return call(service.select, connection_id)

    @router.post("/{connection_id}/disconnect", response_model=ConnectionView)
    def disconnect(connection_id: str):
        return call(service.disconnect, connection_id)

    @router.post("/{connection_id}/delete", response_model=ConnectionList)
    def delete(connection_id: str, body: DeleteConnection):
        return call(service.delete, connection_id, body.expected_revision)

    access = ConnectionAccess(
        *(
            getattr(service, name)
            for name in (
                "profiles",
                "profile",
                "channel",
                "subscribe",
                "instruments",
                "account",
                "listen",
                "supports",
            )
        )
    )
    return Activation(routers=(router,), exports={CONNECTIONS: access}, close=service.close)


plugin = Plugin(
    "asterion.connections",
    ("asterion.identity",),
    activate,
    provides=(CONNECTIONS,),
    consumes=(ACCOUNT_ACCESS,),
    resources=(DATA_ROOT, CREDENTIALS, CONNECTOR_OWNERS),
    observes=("connections.connectors",),
)
