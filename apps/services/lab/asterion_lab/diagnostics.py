"""Backtest attribution, trade diagnostics, and CombinedStrategy observability."""
from collections import deque
import math

from asterion_strategy import CombinedStrategy, product_sector

R = 10 ** 8


def _effective_tick(contract: dict, day: str) -> float:
    tick = contract["price_increment"]
    for change_day, next_tick in contract.get("increments", ()):
        if change_day <= day:
            tick = next_tick
        else:
            break
    return tick / R


def _safe_float(value) -> float:
    return float(value) if value is not None else 0.0


def _finite(value: float | None) -> float | None:
    return value if value is not None and math.isfinite(value) else None


class ComponentTracker:
    """Tracks per-step readiness, weights, and proxy return contribution of CombinedStrategy."""

    def __init__(self, strategy: CombinedStrategy, rolls: dict[str, float]):
        self.strategy = strategy
        self.rolls = dict(rolls)
        self.prev_close: dict[str, float] = {}
        self.prev_contract: dict[str, str] = {}
        self.prev_contrib: list[dict[str, float]] = []
        self.stats: list[dict] = []

    def observe(self, bars: list, _wanted: dict):
        # Compute roll-adjusted per-symbol return since the previous observed bar.
        bar_returns: dict[str, float] = {}
        for bar in bars:
            prev = self.prev_close.get(bar.symbol)
            if prev is not None and prev > 0 and math.isfinite(bar.close):
                if self.prev_contract.get(bar.symbol, bar.contract) != bar.contract:
                    prev *= self.rolls.get(bar.contract, 1.0)
                if prev > 0:
                    bar_returns[bar.symbol] = bar.close / prev - 1.0
            if math.isfinite(bar.close) and bar.close > 0:
                self.prev_close[bar.symbol] = bar.close
                self.prev_contract[bar.symbol] = bar.contract

        components = getattr(self.strategy, "last_components", ())
        if not components:
            return
        if not self.stats:
            self.stats = [
                {
                    "name": item["name"],
                    "total_bars": 0,
                    "ready_bars": 0,
                    "active_bars": 0,
                    "share_sum": 0.0,
                    "gross_sum": 0.0,
                    "contrib_sum": 0.0,
                    "proxy_return": 0.0,
                }
                for item in components
            ]
            self.prev_contrib = [{} for _ in components]

        for idx, item in enumerate(components):
            stat = self.stats[idx]
            prev_w = self.prev_contrib[idx]
            if prev_w and bar_returns:
                stat["proxy_return"] += sum(
                    weight * bar_returns[sym]
                    for sym, weight in prev_w.items()
                    if sym in bar_returns
                )
            share = float(item["share"])
            ready = bool(item["ready"])
            weights = item.get("weights") or {}
            gross = sum(abs(float(w)) for w in weights.values())
            stat["total_bars"] += 1
            if ready:
                stat["ready_bars"] += 1
            if gross > 0:
                stat["active_bars"] += 1
            stat["share_sum"] += share
            stat["gross_sum"] += gross
            stat["contrib_sum"] += abs(share) * gross
            self.prev_contrib[idx] = (
                {sym: share * float(w) for sym, w in weights.items() if float(w) != 0.0}
                if ready
                else {}
            )

    def summary(self) -> list[dict]:
        out = []
        for stat in self.stats:
            total = stat["total_bars"]
            out.append({
                "name": stat["name"],
                "share": stat["share_sum"] / total if total else 0.0,
                "ready_rate": stat["ready_bars"] / total if total else 0.0,
                "active_bars": stat["active_bars"],
                "total_bars": total,
                "avg_gross_weight": stat["gross_sum"] / total if total else 0.0,
                "avg_contribution_weight": stat["contrib_sum"] / total if total else 0.0,
                "proxy_return": stat["proxy_return"],
            })
        return out


def compute(
    config,
    contracts: list[dict],
    run_tables: dict,
    components: list[dict] | None = None,
) -> dict:
    """Compute symbol/sector/direction attribution, trade quality, cost and exposure diagnostics."""
    by_contract = {item["symbol"]: item for item in contracts}
    multipliers = {item["symbol"]: item["multiplier"] / R for item in contracts}
    run_symbol_of = {item["symbol"]: item["run_symbol"] for item in contracts}
    slippage_ticks = int(getattr(config, "slippage_ticks", 0) or 0)
    deposit = float(config.deposit)

    # Latest settlement prices for end-of-run open positions.
    last_settle: dict[str, float] = {}
    positions_df = run_tables["positions"]
    daily_gross_notional: dict[str, float] = {}
    daily_net_notional: dict[str, float] = {}
    for row in positions_df.iter_rows(named=True):
        day = row["trading_day"].isoformat()
        contract = row["contract"]
        long_qty = _safe_float(row["long"])
        short_qty = _safe_float(row["short"])
        settle = _safe_float(row["settlement_price"])
        mult = multipliers.get(contract, 1.0)
        if settle > 0:
            last_settle[contract] = settle
        daily_gross_notional[day] = (
            daily_gross_notional.get(day, 0.0) + (long_qty + short_qty) * settle * mult
        )
        daily_net_notional[day] = (
            daily_net_notional.get(day, 0.0) + (long_qty - short_qty) * settle * mult
        )

    # FIFO round-trip matching per (contract, direction).
    queues: dict[tuple[str, str], deque] = {}
    symbol_acc: dict[str, dict] = {}
    round_trips: list[dict] = []
    total_fees = 0.0
    total_slippage = 0.0
    total_notional = 0.0

    def acc_for(symbol: str) -> dict:
        entry = symbol_acc.get(symbol)
        if entry is None:
            entry = {
                "symbol": symbol,
                "sector": product_sector(symbol) or "other",
                "gross_pnl": 0.0,
                "fees": 0.0,
                "slippage_cost": 0.0,
                "net_pnl": 0.0,
                "long_pnl": 0.0,
                "short_pnl": 0.0,
                "fills": 0,
                "traded_notional": 0.0,
            }
            symbol_acc[symbol] = entry
        return entry

    fills_df = run_tables["fills"]
    for row in fills_df.iter_rows(named=True):
        contract = row["contract"]
        symbol = run_symbol_of.get(contract, contract)
        side = row["side"]
        offset = row["offset"]
        qty = _safe_float(row["quantity"])
        price = _safe_float(row["price"])
        fee = _safe_float(row["fee"])
        day = row["trading_day"]
        day_str = day.isoformat()
        mult = multipliers.get(contract, 1.0)
        c_info = by_contract.get(contract)
        tick = _effective_tick(c_info, day_str) if c_info is not None else 0.0
        slip = slippage_ticks * tick * qty * mult
        notional = price * qty * mult

        total_fees += fee
        total_slippage += slip
        total_notional += notional

        acc = acc_for(symbol)
        acc["fills"] += 1
        acc["fees"] += fee
        acc["slippage_cost"] += slip
        acc["traded_notional"] += notional

        if offset == "open":
            direction = "long" if side == "buy" else "short"
            queues.setdefault((contract, direction), deque()).append(
                [qty, price, fee, day]
            )
        else:
            direction = "long" if side == "sell" else "short"
            sign = 1.0 if direction == "long" else -1.0
            q = queues.setdefault((contract, direction), deque())
            remaining = qty
            while remaining > 1e-12 and q:
                open_lot = q[0]
                matched = min(remaining, open_lot[0])
                open_fee_share = open_lot[2] * (matched / open_lot[0])
                close_fee_share = fee * (matched / qty) if qty > 0 else 0.0
                gross = sign * (price - open_lot[1]) * matched * mult
                net = gross - open_fee_share - close_fee_share
                acc["gross_pnl"] += gross
                acc["net_pnl"] += net
                if direction == "long":
                    acc["long_pnl"] += net
                else:
                    acc["short_pnl"] += net
                round_trips.append({
                    "symbol": symbol,
                    "direction": direction,
                    "gross_pnl": gross,
                    "net_pnl": net,
                    "holding_days": max(0, (day - open_lot[3]).days),
                })
                open_lot[0] -= matched
                open_lot[2] -= open_fee_share
                remaining -= matched
                if open_lot[0] <= 1e-12:
                    q.popleft()

    # Attribute mark-to-market unrealized P&L on positions still open at end of run.
    for (contract, direction), q in queues.items():
        if not q:
            continue
        symbol = run_symbol_of.get(contract, contract)
        mult = multipliers.get(contract, 1.0)
        mark = last_settle.get(contract)
        if mark is None:
            continue
        sign = 1.0 if direction == "long" else -1.0
        acc = acc_for(symbol)
        for qty, entry_price, open_fee, _ in q:
            if qty <= 1e-12:
                continue
            gross = sign * (mark - entry_price) * qty * mult
            net = gross - open_fee
            acc["gross_pnl"] += gross
            acc["net_pnl"] += net
            if direction == "long":
                acc["long_pnl"] += net
            else:
                acc["short_pnl"] += net

    # Trade quality statistics across completed round trips.
    wins = [t["net_pnl"] for t in round_trips if t["net_pnl"] > 0]
    losses = [t["net_pnl"] for t in round_trips if t["net_pnl"] < 0]
    trade_count = len(round_trips)
    win_rate = len(wins) / trade_count if trade_count else None
    avg_win = sum(wins) / len(wins) if wins else None
    avg_loss = abs(sum(losses) / len(losses)) if losses else None
    payoff_ratio = (avg_win / avg_loss) if (avg_win is not None and avg_loss and avg_loss > 0) else None
    loss_sum = abs(sum(losses))
    profit_factor = (sum(wins) / loss_sum) if loss_sum > 0 else None
    avg_holding = (
        sum(t["holding_days"] for t in round_trips) / trade_count if trade_count else None
    )

    long_trips = [t for t in round_trips if t["direction"] == "long"]
    short_trips = [t for t in round_trips if t["direction"] == "short"]
    long_win_rate = (
        sum(1 for t in long_trips if t["net_pnl"] > 0) / len(long_trips) if long_trips else None
    )
    short_win_rate = (
        sum(1 for t in short_trips if t["net_pnl"] > 0) / len(short_trips) if short_trips else None
    )

    # Daily equity, margin, leverage, net exposure, and underwater duration.
    settlements_df = run_tables["settlements"]
    peak = deposit
    cur_dd_days = 0
    max_dd_days = 0
    margin_ratios: list[float] = []
    gross_leverages: list[float] = []
    net_exposures: list[float] = []
    equities: list[float] = []
    for row in settlements_df.iter_rows(named=True):
        day_str = row["trading_day"].isoformat()
        eq = _safe_float(row["equity"])
        margin = _safe_float(row["margin"])
        equities.append(eq)
        if eq >= peak:
            peak = eq
            cur_dd_days = 0
        else:
            cur_dd_days += 1
            if cur_dd_days > max_dd_days:
                max_dd_days = cur_dd_days
        if eq > 0:
            margin_ratios.append(margin / eq)
            gross_leverages.append(daily_gross_notional.get(day_str, 0.0) / eq)
            net_exposures.append(daily_net_notional.get(day_str, 0.0) / eq)

    n_days = len(equities)
    mean_equity = sum(equities) / n_days if n_days else deposit
    annual_periods = float(config.annual_periods or 252.0)
    annual_turnover = (
        (total_notional / mean_equity) * (annual_periods / n_days)
        if n_days > 0 and mean_equity > 0
        else None
    )

    by_symbol = sorted(symbol_acc.values(), key=lambda item: abs(item["net_pnl"]), reverse=True)
    sector_map: dict[str, dict] = {}
    for item in by_symbol:
        sec = sector_map.setdefault(
            item["sector"],
            {
                "sector": item["sector"],
                "gross_pnl": 0.0,
                "fees": 0.0,
                "slippage_cost": 0.0,
                "net_pnl": 0.0,
                "long_pnl": 0.0,
                "short_pnl": 0.0,
                "fills": 0,
            },
        )
        for key in ("gross_pnl", "fees", "slippage_cost", "net_pnl", "long_pnl", "short_pnl", "fills"):
            sec[key] += item[key]
    by_sector = sorted(sector_map.values(), key=lambda item: abs(item["net_pnl"]), reverse=True)

    gross_total = sum(item["gross_pnl"] for item in by_symbol)
    net_total = sum(item["net_pnl"] for item in by_symbol)
    return {
        "trades": {
            "count": trade_count,
            "wins": len(wins),
            "losses": len(losses),
            "win_rate": _finite(win_rate),
            "payoff_ratio": _finite(payoff_ratio),
            "profit_factor": _finite(profit_factor),
            "avg_holding_days": _finite(avg_holding),
        },
        "costs": {
            "gross_pnl": gross_total,
            "fees": total_fees,
            "estimated_slippage": total_slippage,
            "net_pnl": net_total,
            "traded_notional": total_notional,
            "annual_turnover": _finite(annual_turnover),
        },
        "exposure": {
            "avg_margin_ratio": _finite(sum(margin_ratios) / len(margin_ratios)) if margin_ratios else None,
            "peak_margin_ratio": _finite(max(margin_ratios)) if margin_ratios else None,
            "avg_gross_leverage": _finite(sum(gross_leverages) / len(gross_leverages)) if gross_leverages else None,
            "peak_gross_leverage": _finite(max(gross_leverages)) if gross_leverages else None,
            "avg_net_exposure": _finite(sum(net_exposures) / len(net_exposures)) if net_exposures else None,
            "max_drawdown_days": max_dd_days,
        },
        "direction": {
            "long_pnl": sum(item["long_pnl"] for item in by_symbol),
            "short_pnl": sum(item["short_pnl"] for item in by_symbol),
            "long_trades": len(long_trips),
            "short_trades": len(short_trips),
            "long_win_rate": _finite(long_win_rate),
            "short_win_rate": _finite(short_win_rate),
        },
        "by_symbol": by_symbol,
        "by_sector": by_sector,
        "components": list(components or []),
    }
