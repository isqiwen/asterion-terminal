"""Futures sector taxonomy, cross-sectional cleaning and liquidity filters."""
import math
import numbers

import numpy as np

from .core import Context

# China futures product -> sector classification for cross-sectional neutralization.
PRODUCT_SECTORS: dict[str, str] = {
    # Black / ferrous & coal
    **{p: "black" for p in ("rb", "hc", "wr", "ss", "i", "j", "jm", "sf", "sm", "zc", "tc")},
    # Nonferrous & battery metals
    **{p: "nonferrous" for p in ("cu", "bc", "al", "ad", "ao", "zn", "pb", "ni", "sn", "si", "ps", "lc")},
    # Precious metals
    **{p: "precious" for p in ("au", "ag", "pt", "pd")},
    # Energy, chemicals, building materials & freight
    **{
        p: "energy_chem"
        for p in (
            "sc", "fu", "lu", "bu", "ru", "nr", "br", "ta", "px", "pf", "pr", "pl",
            "ma", "me", "eg", "eb", "bz", "l", "pp", "v", "pg", "fg", "sa", "sh", "ur", "sp", "op", "ec",
        )
    },
    # Oilseeds & vegetable oils
    **{p: "oilseeds" for p in ("a", "b", "m", "y", "p", "oi", "ro", "rm", "rs", "pk", "so")},
    # Grains, softs, livestock & timber
    **{
        p: "agriculture"
        for p in (
            "c", "cs", "sr", "cf", "cy", "ap", "cj", "jd", "lh", "rr", "jr",
            "ri", "er", "lr", "wh", "ws", "wt", "pm", "bb", "fb", "lg",
        )
    },
    # Equity index & treasury futures
    **{p: "financial" for p in ("if", "ih", "ic", "im", "ts", "tf", "t", "tl")},
}


def product_sector(symbol: str) -> str:
    """Sector label for `EXCHANGE/product[/YYYY-MM]` or bare product code, empty if unknown."""
    parts = symbol.strip().split("/")
    product = (parts[1] if len(parts) >= 2 else parts[0]).lower()
    return PRODUCT_SECTORS.get(product, "")


def cross_section_zscore(scores: dict, clip: float = 3.0) -> dict:
    """Cross-sectional z-score across finite values in `scores`, clipped to [-clip, clip]."""
    valid = [
        (symbol, float(val))
        for symbol, val in scores.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    ]
    if len(valid) < 2:
        return {symbol: 0.0 for symbol, _ in valid}
    arr = np.array([val for _, val in valid], dtype=float)
    std = float(np.std(arr, ddof=1))
    if std <= 0 or not math.isfinite(std):
        return {symbol: 0.0 for symbol, _ in valid}
    z = (arr - float(np.mean(arr))) / std
    if clip > 0:
        z = np.clip(z, -float(clip), float(clip))
    return {symbol: float(z[i]) for i, (symbol, _) in enumerate(valid)}


def cross_section_rank(scores: dict) -> dict:
    """Cross-sectional centered rank in [-1.0, 1.0] across finite values (ties averaged)."""
    valid = [
        (symbol, float(val))
        for symbol, val in scores.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    ]
    n = len(valid)
    if n == 0:
        return {}
    if n == 1:
        return {valid[0][0]: 0.0}
    arr = np.array([val for _, val in valid], dtype=float)
    order = np.argsort(arr, kind="mergesort")
    sorted_vals = arr[order]
    ranks = np.empty(n, dtype=float)
    i = 0
    while i < n:
        j = i + 1
        while j < n and sorted_vals[j] == sorted_vals[i]:
            j += 1
        avg_rank = 0.5 * (i + j - 1)
        ranks[order[i:j]] = avg_rank
        i = j
    scaled = 2.0 * ranks / (n - 1) - 1.0
    return {symbol: float(scaled[idx]) for idx, (symbol, _) in enumerate(valid)}


def winsorize_mad(scores: dict, n: float = 5.0) -> dict:
    """Clip cross-sectional outliers outside `median +- n * 1.4826 * MAD`."""
    if not isinstance(n, numbers.Real) or isinstance(n, bool) or not math.isfinite(n) or n <= 0:
        raise ValueError("n must be a positive finite number")
    valid = [
        (symbol, float(val))
        for symbol, val in scores.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    ]
    if len(valid) < 2:
        return dict(valid)
    arr = np.array([val for _, val in valid], dtype=float)
    med = float(np.median(arr))
    mad = float(np.median(np.abs(arr - med)))
    if mad <= 0.0 or not math.isfinite(mad):
        return dict(valid)
    bound = float(n) * 1.4826 * mad
    clipped = np.clip(arr, med - bound, med + bound)
    return {symbol: float(clipped[i]) for i, (symbol, _) in enumerate(valid)}


def sector_neutralize(scores: dict, sectors: dict | None = None) -> dict:
    """Demean cross-sectional scores within each futures sector."""
    valid = [
        (symbol, float(val))
        for symbol, val in scores.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    ]
    groups: dict[str, list[float]] = {}
    labels: dict[str, str] = {}
    for symbol, val in valid:
        sector = sectors[symbol] if sectors and symbol in sectors else product_sector(symbol)
        labels[symbol] = sector
        groups.setdefault(sector, []).append(val)
    means = {sector: float(np.mean(vals)) for sector, vals in groups.items()}
    return {symbol: val - means[labels[symbol]] for symbol, val in valid}


def orthogonalize(target: dict, controls) -> dict:
    """Cross-sectional OLS residual of `target` regressed on one or more control score dicts."""
    control_list = [controls] if isinstance(controls, dict) else list(controls)
    valid_target = {
        symbol: float(val)
        for symbol, val in target.items()
        if isinstance(val, numbers.Real) and not isinstance(val, bool) and math.isfinite(val)
    }
    if not control_list:
        return valid_target
    common = [
        symbol
        for symbol in valid_target
        if all(
            symbol in ctrl
            and isinstance(ctrl[symbol], numbers.Real)
            and not isinstance(ctrl[symbol], bool)
            and math.isfinite(ctrl[symbol])
            for ctrl in control_list
        )
    ]
    if len(common) < len(control_list) + 2:
        return {}
    y = np.array([valid_target[symbol] for symbol in common], dtype=float)
    x = np.column_stack(
        [
            np.ones(len(common), dtype=float),
            *([float(ctrl[symbol]) for symbol in common] for ctrl in control_list),
        ]
    )
    beta, *_ = np.linalg.lstsq(x, y, rcond=None)
    resid = y - x @ beta
    return {symbol: float(resid[i]) for i, symbol in enumerate(common)}


def liquid_symbols(
    ctx: Context,
    bars: dict,
    min_bars: int = 1,
    min_volume: float = 0.0,
    min_open_interest: float = 0.0,
    window: int = 20,
) -> list:
    """Symbols trading at `ctx.time` with sufficient history, volume and open interest."""
    if min_bars < 1 or window < 1:
        raise ValueError("min_bars and window must be at least 1")
    eligible = []
    for symbol in ctx.symbols:
        bar = bars.get(symbol)
        if bar is None or not math.isfinite(bar.close) or bar.close <= 0:
            continue
        closes = ctx.history(symbol, "close", min_bars)
        if len(closes) < min_bars or not np.all(np.isfinite(closes)):
            continue
        if min_volume > 0:
            vols = ctx.history(symbol, "volume", window)
            finite_vols = vols[np.isfinite(vols)]
            if len(finite_vols) == 0 or float(np.mean(finite_vols)) < min_volume:
                continue
        if min_open_interest > 0:
            ois = ctx.history(symbol, "open_interest", window)
            finite_ois = ois[np.isfinite(ois)]
            if len(finite_ois) == 0 or float(np.mean(finite_ois)) < min_open_interest:
                continue
        eligible.append(symbol)
    return eligible
