"""Source-independent market functionality."""

import asyncio
from contextlib import asynccontextmanager
from threading import RLock

from fastapi import APIRouter, Depends, HTTPException

from asterion.connections.public import CONNECTIONS, Exchange
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.market.contracts import ContractChoicesService
from asterion.market.models import ContractChoices, MarketState, Watchlist
from asterion.market.service import MarketService
from asterion.platform.diagnostics import ServiceState
from asterion.platform.plugins import Activation, Plugin
from asterion.platform.resources import DATA_ROOT

from .public import CONTRACT_NAMES, ContractNames


def activate(context):
    access = context.require(CONNECTIONS)
    root = context.resource(DATA_ROOT) / "market" / "connections"
    services, lock = {}, RLock()

    def service(connection_id):
        access.profile(connection_id)
        with lock:
            if connection_id not in services:
                directory = ContractChoicesService(
                    root / connection_id, lambda cancel: access.instruments(connection_id, cancel)
                )
                services[connection_id] = MarketService(
                    root / connection_id, connection_id, access, directory
                )
            return services[connection_id]

    def event(connection_id, generation, kind, value):
        item = services.get(connection_id)
        if item is not None:
            item.event(generation, kind, value)

    remove = access.listen(event)

    async def maintain(stop):
        while not stop.is_set():
            current = set(access.profiles())
            with lock:
                retired = [services.pop(key) for key in set(services) - current]
            for item in retired:
                await asyncio.to_thread(item.directory.stop)
            for connection_id in current:
                try:
                    await asyncio.to_thread(service(connection_id).maintain_contracts)
                except Exception:  # noqa: BLE001
                    item = services.get(connection_id)
                    if item is not None:
                        with item.directory.lock:
                            item.directory.state = "error"
                            item.directory.detail = "目录维护失败，已保留本地资料；请检查连接后重试"
            try:
                await asyncio.wait_for(stop.wait(), 2)
            except TimeoutError:
                pass

    @asynccontextmanager
    async def lifespan(app):
        for connection_id in access.profiles():
            service(connection_id)
        stop = asyncio.Event()
        task = asyncio.create_task(maintain(stop))
        try:
            yield
        finally:
            stop.set()
            await task

    router = APIRouter(
        prefix="/api/v1/market",
        dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)],
        lifespan=lifespan,
    )

    def get(connection_id):
        try:
            return service(connection_id)
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    @router.get("/state", response_model=MarketState)
    def state(connection_id: str):
        return get(connection_id).snapshot()

    @router.get("/contracts", response_model=ContractChoices)
    def contracts(connection_id: str, exchange: Exchange):
        return get(connection_id).directory.choices(exchange)

    @router.post("/contracts/refresh", response_model=ContractChoices)
    def refresh(connection_id: str, exchange: Exchange):
        get(connection_id).refresh_contracts()
        return get(connection_id).directory.choices(exchange)

    @router.post("/watchlist", response_model=MarketState)
    def watchlist(connection_id: str, body: Watchlist):
        try:
            return get(connection_id).watchlist(body)
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    def close():
        remove()
        for item in list(services.values()):
            item.directory.stop()

    def connected(connection_id):
        try:
            return access.channel(connection_id, "market").state == "ready"
        except ValueError:
            return False

    def status(ready):
        return [
            ServiceState(
                id="market",
                name="实时行情",
                state="ready" if any(connected(k) for k in access.profiles()) else "unconfigured",
                detail="通过连接插件接入行情",
            )
        ]

    return Activation(
        routers=(router,),
        close=close,
        hooks={"services": (status,)},
        exports={
            CONTRACT_NAMES: ContractNames(
                lambda connection_id: service(connection_id).directory.names()
            )
        },
    )


plugin = Plugin(
    "asterion.market",
    ("asterion.identity", "asterion.connections"),
    activate,
    consumes=(ACCOUNT_ACCESS, CONNECTIONS),
    provides=(CONTRACT_NAMES,),
    resources=(DATA_ROOT,),
)
