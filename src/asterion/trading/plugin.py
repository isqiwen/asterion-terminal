"""Account functionality over the public connection contract."""

import asyncio
from contextlib import asynccontextmanager
from threading import RLock

from fastapi import APIRouter, Depends, HTTPException

from asterion.connections.public import CONNECTIONS
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.market.public import CONTRACT_NAMES
from asterion.platform.diagnostics import ServiceState
from asterion.platform.plugins import Activation, Plugin
from asterion.trading.observation import AccountService, AccountState


def activate(context):
    access, names = context.require(CONNECTIONS), context.require(CONTRACT_NAMES)
    services = {}
    lock = RLock()

    def service(connection_id):
        access.profile(connection_id)
        with lock:
            if connection_id not in services:
                services[connection_id] = AccountService(access, connection_id, names.read)
            return services[connection_id]

    def refresh_profile(connection_id):
        try:
            service(connection_id).refresh()
        except ValueError:
            # A disconnected profile can be deleted between enumeration and refresh.
            pass

    def ready_profile(connection_id):
        try:
            return service(connection_id).snapshot().state == "ready"
        except ValueError:
            return False

    @asynccontextmanager
    async def lifespan(app):
        stop = asyncio.Event()

        async def poll():
            while not stop.is_set():
                current = set(access.profiles())
                with lock:
                    for key in set(services) - current:
                        services.pop(key).cancel.set()
                await asyncio.gather(*(asyncio.to_thread(refresh_profile, k) for k in current))
                try:
                    await asyncio.wait_for(stop.wait(), 2)
                except TimeoutError:
                    pass

        task = asyncio.create_task(poll())
        try:
            yield
        finally:
            stop.set()
            for item in list(services.values()):
                item.cancel.set()
            await task

    router = APIRouter(
        prefix="/api/v1/trading",
        dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)],
        lifespan=lifespan,
    )

    def get(connection_id):
        try:
            return service(connection_id)
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    @router.get("/account", response_model=AccountState)
    def account(connection_id: str):
        return get(connection_id).snapshot()

    @router.post("/account/refresh", response_model=AccountState)
    def refresh(connection_id: str):
        return get(connection_id).refresh(force=True)

    return Activation(
        routers=(router,),
        hooks={
            "services": (
                lambda ready: [
                    ServiceState(
                        id="trading",
                        name="账户（只读）",
                        state="configured"
                        if any(ready_profile(k) for k in access.profiles())
                        else "unavailable",
                        detail="交易执行未开放",
                    )
                ],
            )
        },
    )


plugin = Plugin(
    "asterion.trading",
    ("asterion.identity", "asterion.connections", "asterion.market"),
    activate,
    consumes=(ACCOUNT_ACCESS, CONNECTIONS, CONTRACT_NAMES),
)
