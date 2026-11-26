"""Single-instance multi-strategy weight combiner."""
import math
import numbers

import numpy as np

from .core import Context, Strategy
from .portfolio import cap_sector_weights, combine_weights, erc_weights, hrp_weights, smooth_weights


class _SubView:
    def __init__(self, parent: Context):
        self.parent = parent
        self.mode = None
        self.wanted = {}

    @property
    def symbols(self):
        return self.parent.symbols

    @property
    def time(self):
        return self.parent.time

    @property
    def contracts(self):
        return self.parent.contracts

    def history(self, symbol: str, field: str, count: int | None = None) -> np.ndarray:
        return self.parent.history(symbol, field, count)

    def factor(self, function, symbol: str, count: int | None = None, **settings) -> float:
        return self.parent.factor(function, symbol, count, **settings)

    def weights(self, weights: dict):
        if self.mode not in (None, "weights"):
            raise RuntimeError("CombinedStrategy sub-strategies must output weights")
        self.mode = "weights"
        out = {}
        for symbol, weight in weights.items():
            if symbol not in self.parent.columns:
                raise KeyError(f"{symbol} is not traded by this run")
            if (
                not isinstance(weight, numbers.Real)
                or isinstance(weight, bool)
                or not math.isfinite(weight)
            ):
                raise TypeError(f"weight of {symbol} is not a finite number: {weight!r}")
            out[symbol] = float(weight)
        self.wanted = {symbol: out.get(symbol, 0.0) for symbol in self.parent.symbols}

    def target(self, symbol: str, lots: int):
        raise RuntimeError("CombinedStrategy sub-strategies must output weights, not lots")


class CombinedStrategy(Strategy):
    """Standard single-instance combiner merging target weights of sub-strategies.

    Subclasses may set `allocation` to `"fixed"`, `"vol_balance"` (or
    `vol_balance=True`), `"hrp"` (causal Hierarchical Risk Parity with
    Ledoit-Wolf shrinkage across component proxy returns), or `"erc"` (causal
    Equal Risk Contribution / Risk Parity with Ledoit-Wolf shrinkage),
    `max_sector_gross` / `max_sector_net` for sector exposure caps, and
    `smooth_alpha` / `deadband` for portfolio-level turnover control.
    """

    allocation: str = "fixed"
    vol_balance: bool = False
    vol_window: int = 20
    max_sector_gross: float = 0.0
    max_sector_net: float = 0.0
    smooth_alpha: float = 1.0
    deadband: float = 0.0

    def components(self) -> list:
        raise NotImplementedError

    def __init__(self, **values):
        super().__init__(**values)
        if self.allocation not in ("fixed", "vol_balance", "hrp", "erc"):
            raise ValueError("CombinedStrategy allocation must be 'fixed', 'vol_balance', 'hrp', or 'erc'")
        if self.allocation in ("hrp", "erc"):
            self._alloc_mode = self.allocation
        elif self.vol_balance or self.allocation == "vol_balance":
            self._alloc_mode = "vol_balance"
        else:
            self._alloc_mode = "fixed"
        items = list(self.components())
        if not items:
            raise ValueError("CombinedStrategy requires at least one component strategy")
        self._subs = []
        for sub, share in items:
            if not isinstance(sub, Strategy):
                raise TypeError("each component must be a Strategy instance")
            if (
                not isinstance(share, numbers.Real)
                or isinstance(share, bool)
                or not math.isfinite(share)
                or share < 0
            ):
                raise ValueError(f"component share must be a finite nonnegative number: {share!r}")
            self._subs.append((sub, float(share)))
        self._standing = [{} for _ in self._subs]
        self._was_ready = [False for _ in self._subs]
        self._all_ready = False
        self._comp_returns: list[list[float]] = [[] for _ in self._subs]
        self._prev_combined: dict[str, float] = {}
        self.last_components: list[dict] = []
        if type(self).history_limit is None:
            sub_limits = [getattr(sub, "history_limit", None) for sub, _ in self._subs]
            if all(limit is not None for limit in sub_limits):
                self.history_limit = max(2, max(sub_limits))

    def ready(self, ctx: Context) -> bool:
        return self._all_ready

    def _effective_shares(self) -> list[float]:
        base = [share for _, share in self._subs]
        if self._alloc_mode == "fixed":
            return base
        window = max(2, int(self.vol_window))
        total_share = sum(base)
        if total_share <= 0:
            return base
        active = [idx for idx, share in enumerate(base) if share > 0]
        if self._alloc_mode in ("hrp", "erc"):
            if len(active) < 2:
                return base
            cov_window = max(5, window)
            matrix = np.column_stack(
                [np.asarray(self._comp_returns[idx], dtype=float) for idx in active]
            )
            matrix = matrix[np.all(np.isfinite(matrix), axis=1)][-cov_window:]
            if len(matrix) < cov_window:
                return base
            raw_alloc = (
                hrp_weights(matrix, shrinkage=True)
                if self._alloc_mode == "hrp"
                else erc_weights(matrix, shrinkage=True)
            )
            tilted = [base[idx] * float(raw_alloc[pos]) for pos, idx in enumerate(active)]
            denom = sum(tilted)
            if denom <= 0 or not math.isfinite(denom):
                return base
            out = [0.0 for _ in base]
            for pos, idx in enumerate(active):
                out[idx] = total_share * tilted[pos] / denom
            return out
        inv_vols = []
        for idx, share in enumerate(base):
            if share <= 0:
                inv_vols.append(0.0)
                continue
            hist = self._comp_returns[idx]
            if len(hist) < window:
                return base
            vol = float(np.std(hist[-window:], ddof=1))
            if not math.isfinite(vol) or vol <= 1e-12:
                return base
            inv_vols.append(share / vol)
        denom = sum(inv_vols)
        if denom <= 0:
            return base
        return [total_share * w / denom for w in inv_vols]

    def on_bars(self, ctx: Context, bars: dict):
        # Update strictly causal component return proxies from roll-adjusted closes before stepping.
        if self._alloc_mode != "fixed":
            bar_returns = {}
            for sym in bars:
                closes = ctx.history(sym, "close", 2)
                if (
                    len(closes) == 2
                    and closes[0] > 0
                    and math.isfinite(closes[0])
                    and math.isfinite(closes[1])
                ):
                    bar_returns[sym] = float(closes[1] / closes[0] - 1.0)
            if bar_returns:
                max_keep = max(256, 4 * max(2, int(self.vol_window)))
                for idx in range(len(self._subs)):
                    ret = math.nan
                    if self._was_ready[idx] and self._standing[idx]:
                        ret = sum(
                            w * bar_returns[s]
                            for s, w in self._standing[idx].items()
                            if s in bar_returns
                        )
                    elif self._alloc_mode == "vol_balance":
                        continue
                    hist = self._comp_returns[idx]
                    hist.append(ret)
                    if len(hist) > max_keep:
                        del hist[:-max_keep]

        changed = False
        ready_flags = []
        for idx, (sub, _) in enumerate(self._subs):
            view = _SubView(ctx)
            sub.on_bars(view, bars)
            is_ready = sub.ready(view)
            if type(is_ready) is not bool:
                raise TypeError("strategy readiness must be a bool")
            ready_flags.append(is_ready)
            if not is_ready:
                if self._standing[idx] or self._was_ready[idx]:
                    changed = True
                self._standing[idx] = {}
                self._was_ready[idx] = False
                continue
            if not self._was_ready[idx]:
                self._was_ready[idx] = True
                changed = True
            if view.wanted:
                self._standing[idx] = view.wanted
                changed = True
        self._all_ready = all(ready_flags)
        shares = self._effective_shares()
        self.last_components = [
            {
                "name": type(sub).__name__,
                "share": shares[idx],
                "ready": bool(self._was_ready[idx]),
                "weights": dict(self._standing[idx]),
            }
            for idx, (sub, _) in enumerate(self._subs)
        ]
        if self._all_ready and (changed or self._alloc_mode != "fixed"):
            combined = combine_weights(
                [(self._standing[i], shares[i]) for i in range(len(self._subs))],
                ctx.symbols,
            )
            if self.max_sector_gross > 0 or self.max_sector_net > 0:
                combined = cap_sector_weights(
                    combined,
                    max_sector_gross=self.max_sector_gross,
                    max_sector_net=self.max_sector_net,
                )
            if self.smooth_alpha < 1.0 or self.deadband > 0.0:
                if self._prev_combined:
                    combined = smooth_weights(
                        self._prev_combined,
                        combined,
                        alpha=self.smooth_alpha,
                        deadband=self.deadband,
                    )
                self._prev_combined = dict(combined)
            ctx.weights(combined)
