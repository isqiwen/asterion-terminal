"""Backtest attribution, trade diagnostics, and CombinedStrategy observability."""
from collections import deque
import datetime
import math

from asterion_lab import statistics as lab_stats
from asterion_lab.chart import BEIJING
from asterion_strategy import CombinedStrategy, product_sector

R = 10 ** 8
SESSION_ORDER = ("night", "morning_1", "morning_2", "afternoon", "other", "opening_15m")


def _intraday_session(time_ns: int, contract: str) -> tuple[str, bool]:
    """Classify a fill timestamp (UTC ns) into China futures trading session & opening 15m flag."""
    dt = datetime.datetime.fromtimestamp(time_ns / 1e9, tz=BEIJING)
    hhmm = dt.hour * 60 + dt.minute
    exchange = contract.split("/", 1)[0]
    if hhmm >= 21 * 60 or hhmm <= 2 * 60 + 30:
        session = "night"
    elif 8 * 60 + 55 <= hhmm <= 10 * 60 + 15:
        session = "morning_1"
    elif 10 * 60 + 16 <= hhmm <= 11 * 60 + 35:
        session = "morning_2"
    elif 13 * 60 <= hhmm <= 15 * 60 + 20:
        session = "afternoon"
    else:
        session = "other"
    if exchange == "CFFEX":
        is_open15 = 9 * 60 + 30 <= hhmm <= 9 * 60 + 45
    else:
        is_open15 = (9 * 60 <= hhmm <= 9 * 60 + 15) or (21 * 60 <= hhmm <= 21 * 60 + 15)
    return session, is_open15


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


def _top_drawdowns(days: list[str], equities: list[float], deposit: float, limit: int = 5) -> list[dict]:
    if not days or not equities:
        return []
    peak = deposit if deposit > 0 else equities[0]
    peak_idx = -1 if deposit > 0 and deposit >= equities[0] else 0
    peak_day = days[0]
    trough = peak
    trough_idx = peak_idx
    trough_day = peak_day
    episodes: list[dict] = []

    for idx, (day_str, eq) in enumerate(zip(days, equities)):
        if eq >= peak:
            if peak > 0 and trough < peak:
                episodes.append({
                    "peak_day": peak_day,
                    "trough_day": trough_day,
                    "recovery_day": day_str,
                    "depth": round(trough / peak - 1.0, 6),
                    "peak_to_trough_days": max(1, trough_idx - max(0, peak_idx)),
                    "duration_days": max(1, idx - max(0, peak_idx)),
                })
            peak = eq
            peak_idx = idx
            peak_day = day_str
            trough = eq
            trough_idx = idx
            trough_day = day_str
        elif eq < trough:
            trough = eq
            trough_idx = idx
            trough_day = day_str

    if peak > 0 and trough < peak:
        episodes.append({
            "peak_day": peak_day,
            "trough_day": trough_day,
            "recovery_day": None,
            "depth": round(trough / peak - 1.0, 6),
            "peak_to_trough_days": max(1, trough_idx - max(0, peak_idx)),
            "duration_days": max(1, (len(equities) - 1) - max(0, peak_idx)),
        })
    episodes.sort(key=lambda item: item["depth"])
    return episodes[:limit]


def _equal_weight_benchmark(
    config,
    contracts: list[dict],
    series: list[list[tuple[str, int]]] | None,
    days: list[str],
) -> dict | None:
    if not days or not contracts:
        return None
    settle_maps = [
        {day: price / R for day, price in item.get("settlements", ()) if price and price > 0}
        for item in contracts
    ]
    by_symbol: dict[str, list[tuple[str, int]]] = {}
    if series:
        for schedule in series:
            if schedule:
                sym = contracts[schedule[0][1]]["run_symbol"]
                by_symbol[sym] = sorted(schedule, key=lambda pair: pair[0])
    for idx, item in enumerate(contracts):
        sym = item["run_symbol"]
        if sym not in by_symbol:
            by_symbol[sym] = [("", idx)]

    prev_price: dict[str, float] = {}
    prev_idx: dict[str, int] = {}
    nav: list[float] = []
    bench_returns: list[float] = []
    current_nav = 1.0

    for day_str in days:
        rets: list[float] = []
        for sym, schedule in by_symbol.items():
            active = schedule[0][1]
            for roll_day, c_idx in schedule:
                if roll_day <= day_str:
                    active = c_idx
                else:
                    break
            price = settle_maps[active].get(day_str)
            if price is None or price <= 0:
                continue
            p_prev = prev_price.get(sym)
            i_prev = prev_idx.get(sym)
            if p_prev is not None and p_prev > 0:
                if i_prev is not None and i_prev != active:
                    p_prev *= float(contracts[active].get("roll_ratio", 1.0))
                if p_prev > 0:
                    rets.append(price / p_prev - 1.0)
            prev_price[sym] = price
            prev_idx[sym] = active
        day_ret = sum(rets) / len(rets) if rets else 0.0
        current_nav *= 1.0 + day_ret
        nav.append(round(current_nav, 6))
        if len(nav) > 1:
            bench_returns.append(day_ret)

    peak = 1.0
    max_dd = 0.0
    for val in nav:
        if val > peak:
            peak = val
        if peak > 0:
            dd = val / peak - 1.0
            if dd < max_dd:
                max_dd = dd

    sharpe = None
    annual_periods = getattr(config, "annual_periods", None)
    if annual_periods and getattr(config, "zero_risk_free", False) and len(bench_returns) >= 2:
        mean_r = sum(bench_returns) / len(bench_returns)
        var = sum((r - mean_r) ** 2 for r in bench_returns) / (len(bench_returns) - 1)
        if var > 0:
            sharpe = mean_r / math.sqrt(var) * math.sqrt(float(annual_periods))

    return {
        "days": days,
        "nav": nav,
        "total_return": _finite(nav[-1] - 1.0),
        "max_drawdown": _finite(max_dd),
        "sharpe": _finite(sharpe),
    }


def compute(
    config,
    contracts: list[dict],
    run_tables: dict,
    components: list[dict] | None = None,
    series: list[list[tuple[str, int]]] | None = None,
    trials: dict | None = None,
    uses_pit_fundamentals: bool = False,
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
    track_sessions = int(getattr(config, "minutes", 0) or 0) > 0
    session_acc: dict[str, dict] = {}

    def sess_for(name: str) -> dict:
        entry = session_acc.get(name)
        if entry is None:
            entry = {
                "session": name,
                "trades": 0,
                "wins": 0,
                "gross_pnl": 0.0,
                "fees": 0.0,
                "slippage_cost": 0.0,
                "net_pnl": 0.0,
                "fills": 0,
            }
            session_acc[name] = entry
        return entry

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

        fill_session, fill_open15 = ("", False)
        if track_sessions and row.get("time_ns"):
            fill_session, fill_open15 = _intraday_session(int(row["time_ns"]), contract)
            for key in (fill_session, *(["opening_15m"] if fill_open15 else [])):
                s_entry = sess_for(key)
                s_entry["fills"] += 1
                s_entry["fees"] += fee
                s_entry["slippage_cost"] += slip

        if offset == "open":
            direction = "long" if side == "buy" else "short"
            queues.setdefault((contract, direction), deque()).append(
                [qty, price, fee, day, fill_session, fill_open15]
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
                if track_sessions and open_lot[4]:
                    for key in (open_lot[4], *(["opening_15m"] if open_lot[5] else [])):
                        s_entry = sess_for(key)
                        s_entry["trades"] += 1
                        if net > 0:
                            s_entry["wins"] += 1
                        s_entry["gross_pnl"] += gross
                        s_entry["net_pnl"] += net
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
        for qty, entry_price, open_fee, *_ in q:
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

    # Daily equity, margin, leverage, net exposure, return, and underwater duration.
    settlements_df = run_tables["settlements"]
    peak = deposit
    cur_dd_days = 0
    max_dd_days = 0
    daily_days: list[str] = []
    margin_ratios: list[float] = []
    gross_leverages: list[float] = []
    net_exposures: list[float] = []
    daily_returns: list[float] = []
    equities: list[float] = []
    prev_eq = deposit
    for row in settlements_df.iter_rows(named=True):
        day_str = row["trading_day"].isoformat()
        eq = _safe_float(row["equity"])
        margin = _safe_float(row["margin"])
        daily_days.append(day_str)
        equities.append(eq)
        daily_returns.append(round(eq / prev_eq - 1.0, 6) if prev_eq > 0 else 0.0)
        prev_eq = eq
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
        else:
            margin_ratios.append(0.0)
            gross_leverages.append(0.0)
            net_exposures.append(0.0)

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
    significance = None
    if len(daily_returns) >= 4:
        trial_count = max(1, int((trials or {}).get("strategy_runs") or 1))
        nw = lab_stats.newey_west_tstat(daily_returns)
        sig_entry: dict = {
            "n_days": len(daily_returns),
            "trials": trial_count,
            "nw_t": _finite(nw["t"]) if nw else None,
            "nw_p": _finite(nw["p_value"]) if nw else None,
        }
        if getattr(config, "annual_periods", None) and getattr(config, "zero_risk_free", False):
            dsr_res = lab_stats.deflated_sharpe_ratio(
                daily_returns,
                trials=trial_count,
                annual_periods=float(config.annual_periods),
            )
            if dsr_res is not None:
                sig_entry["skewness"] = _finite(dsr_res["skewness"])
                sig_entry["excess_kurtosis"] = _finite(dsr_res["excess_kurtosis"])
                sig_entry["psr"] = _finite(dsr_res["psr"])
                sig_entry["dsr"] = _finite(dsr_res["dsr"])
                sig_entry["expected_max_sharpe"] = _finite(dsr_res["expected_max_sharpe"])
            if len(daily_returns) >= 20:
                boot = lab_stats.block_bootstrap_ci(
                    daily_returns,
                    annual_periods=float(config.annual_periods),
                )
                if boot is not None:
                    sig_entry["bootstrap"] = boot
        significance = sig_entry

    result = {
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
        "daily": {
            "days": daily_days,
            "margin_ratio": [round(v, 4) for v in margin_ratios],
            "gross_leverage": [round(v, 4) for v in gross_leverages],
            "net_exposure": [round(v, 4) for v in net_exposures],
            "daily_return": daily_returns,
        },
        "drawdowns": _top_drawdowns(daily_days, equities, deposit),
        "benchmark": _equal_weight_benchmark(config, contracts, series, daily_days),
        "by_symbol": by_symbol,
        "by_sector": by_sector,
        "components": list(components or []),
        "uses_pit_fundamentals": bool(uses_pit_fundamentals),
    }
    by_session = []
    if track_sessions and session_acc:
        for name in SESSION_ORDER:
            entry = session_acc.get(name)
            if entry is not None and (entry["fills"] > 0 or entry["trades"] > 0):
                by_session.append({
                    **entry,
                    "win_rate": _finite(entry["wins"] / entry["trades"]) if entry["trades"] else None,
                })

    if by_session:
        result["by_session"] = by_session
    if significance is not None:
        result["significance"] = significance
    if trials is not None:
        result["trials"] = trials
    return result
