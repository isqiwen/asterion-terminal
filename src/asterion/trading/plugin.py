"""Explicit unavailable trading contribution; never enables order execution."""

from asterion.platform.diagnostics import ServiceState
from asterion.platform.plugins import Activation, Plugin


def activate(context):
    return Activation(
        hooks={
            "services": (
                lambda ready: [
                    ServiceState(
                        id="trading",
                        name="交易网关",
                        state="not_integrated",
                        detail="当前为研究环境，尚未接入交易柜台",
                    )
                ],
            )
        }
    )


plugin = Plugin("asterion.trading", (), activate)
