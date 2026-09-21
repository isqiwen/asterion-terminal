"""Self-contained SMA strategy; all implementation code is included in its digest."""

from hashlib import sha256
from pathlib import Path

from asterion.platform.plugins import Activation, Capability, Plugin
from asterion.research.public import (
    ClosedBar,
    IntegerParameter,
    Strategy,
    StrategyInfo,
    StrategyRef,
)


class Session:
    def __init__(self, parameters):
        self.fast, self.slow = parameters["fast"], parameters["slow"]
        self.closes = []

    def close(self):
        self.closes.clear()

    def on_close(self, bar: ClosedBar) -> bool:
        self.closes.append(bar.close)
        self.closes = self.closes[-self.slow :]
        return len(self.closes) >= self.slow and (
            sum(self.closes[-self.fast :]) / self.fast > sum(self.closes) / self.slow
        )


def validate(parameters):
    if parameters["fast"] >= parameters["slow"]:
        raise ValueError("快均线周期必须小于慢均线周期")


def contribution():
    return Strategy(
        StrategyInfo(
            identity=StrategyRef(
                id="builtin.sma-long",
                version="1.0.0",
                digest=sha256(Path(__file__).read_bytes()).hexdigest(),
            ),
            name="双均线",
            description="快均线高于慢均线时持有多仓，否则空仓；预热为慢均线周期。",
            parameters=(
                IntegerParameter(key="fast", label="快均线周期", minimum=1, maximum=250, default=5),
                IntegerParameter(
                    key="slow", label="慢均线周期", minimum=2, maximum=500, default=20
                ),
            ),
        ),
        validate,
        lambda p: int(p["slow"]),
        Session,
    )


CAPABILITY = Capability("strategy.sma", "asterion.strategy_sma", Strategy)
plugin = Plugin(
    "asterion.strategy_sma",
    (),
    lambda _: Activation(exports={CAPABILITY: contribution()}),
    provides=(CAPABILITY,),
)
