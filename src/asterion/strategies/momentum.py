"""Close-to-close momentum, independently contributed through the strategy contract."""

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
        self.lookback = parameters["lookback"]
        self.closes = []

    def close(self):
        self.closes.clear()

    def on_close(self, bar: ClosedBar) -> bool:
        self.closes.append(bar.close)
        self.closes = self.closes[-(self.lookback + 1) :]
        return len(self.closes) > self.lookback and self.closes[-1] > self.closes[0]


def contribution():
    return Strategy(
        StrategyInfo(
            identity=StrategyRef(
                id="builtin.momentum-long",
                version="1.0.0",
                digest=sha256(Path(__file__).read_bytes()).hexdigest(),
            ),
            name="收盘动量",
            description="收盘价高于回看周期前的收盘价时持有多仓，否则空仓；预热为回看周期加一。",
            parameters=(
                IntegerParameter(key="lookback", label="回看周期", minimum=1, maximum=499, default=10),
            ),
        ),
        lambda _: None,
        lambda p: int(p["lookback"]) + 1,
        Session,
    )


CAPABILITY = Capability("strategy.momentum", "asterion.strategy_momentum", Strategy)
plugin = Plugin(
    "asterion.strategy_momentum",
    (),
    lambda _: Activation(exports={CAPABILITY: contribution()}),
    provides=(CAPABILITY,),
)
