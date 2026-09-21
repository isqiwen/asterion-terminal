"""SimNow market-data functionality; no trading or account execution."""

from fastapi import APIRouter, Depends, HTTPException

from asterion.identity.public import ACCOUNT_ACCESS
from asterion.market.ctp import CtpFeed
from asterion.market.models import MarketConfiguration, MarketConnect, MarketState
from asterion.market.service import MarketService
from asterion.platform.diagnostics import ServiceState
from asterion.platform.plugins import Activation, Plugin
from asterion.platform.resources import DATA_ROOT


def activate(context):
    service = MarketService(context.resource(DATA_ROOT), CtpFeed)
    router = APIRouter(
        prefix="/api/v1/market", dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)]
    )

    @router.get("/state", response_model=MarketState)
    def state():
        return service.snapshot()

    @router.post("/configuration", response_model=MarketState)
    def configure(body: MarketConfiguration):
        try:
            return service.configure(body)
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    @router.post("/connect", response_model=MarketState)
    def connect(body: MarketConnect):
        try:
            return service.connect(body.password.get_secret_value())
        except ValueError as error:
            raise HTTPException(409, str(error)) from None

    @router.post("/disconnect", response_model=MarketState)
    def disconnect():
        return service.disconnect()

    def status(ready):
        state = service.snapshot()
        return [
            ServiceState(
                id="market",
                name="SimNow 行情",
                state="ready"
                if state.state == "connected"
                else "unavailable"
                if state.state == "error"
                else "configured"
                if state.configuration.front
                else "unconfigured",
                detail=state.detail,
            )
        ]

    def close():
        service.disconnect()

    return Activation(routers=(router,), hooks={"services": (status,)}, close=close)


plugin = Plugin(
    "asterion.market",
    ("asterion.identity",),
    activate,
    consumes=(ACCOUNT_ACCESS,),
    resources=(DATA_ROOT,),
)
