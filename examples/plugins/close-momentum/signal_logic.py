from decimal import Decimal


def warmup(parameters):
    if (
        set(parameters) != {"lookback", "threshold", "enabled", "comparison"}
        or type(parameters["lookback"]) is not int
    ):
        raise ValueError("Explicit integer lookback required")
    if not 1 <= parameters["lookback"] <= 499:
        raise ValueError("Lookback out of range")
    return parameters["lookback"] + 1


class Momentum:
    def __init__(self, parameters):
        self.length = warmup(parameters)
        self.closes = []
        self.threshold = Decimal(parameters["threshold"])
        self.enabled = parameters["enabled"]
        self.inclusive = parameters["comparison"] == "inclusive"

    def on_close(self, trading_day, close):
        self.closes.append(close)
        self.closes = self.closes[-self.length :]
        if not self.enabled or len(self.closes) != self.length:
            return False
        target = self.closes[0] * (1 + self.threshold)
        return self.closes[-1] >= target if self.inclusive else self.closes[-1] > target
