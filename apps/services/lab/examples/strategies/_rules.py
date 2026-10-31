"""What the example rules share: exact prices and the sides a rule may hold."""
import numpy as np

SIDES = ("both", "long", "short")


def units(values) -> list:
    """Prices as the integers of eight-place decimals, exact for every price a
    contract trades at, so that rules compare them without rounding."""
    return [int(value) for value in np.rint(np.asarray(values) * 1e8)]


def lots(side: int, params: dict) -> int:
    """What a rule holds when it takes `side`: its lots on a side it may hold,
    nothing on one it may not."""
    if params["sides"] not in SIDES:
        raise ValueError(f"sides is one of {', '.join(SIDES)}")
    if side > 0 and params["sides"] != "short":
        return params["lots"]
    if side < 0 and params["sides"] != "long":
        return -params["lots"]
    return 0
