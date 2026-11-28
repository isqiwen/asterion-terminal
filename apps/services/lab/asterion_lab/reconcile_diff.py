"""Backtest replay and bar/decision/fill discrepancy comparisons for live reconciliation."""

from decimal import Decimal
from itertools import groupby
import math
from operator import itemgetter

from asterion_lab import backtest, chart, data as lab_data
from asterion_lab.reconcile_live import format_decimal, parse_decimal
from asterion_strategy import Context, Strategy, quantize_weight, step

BAR_PRICE_FIELDS = ("open", "high", "low", "close")


def replay_backtest(
    dataset,
    config: backtest.Config,
    strategy: Strategy,
    *,
    begin_ms: int | None = None,
    end_ms: int | None = None,
    execute_from_ms: int | None = None,
) -> dict:
    """Run Lab backtest replay and capture bar-by-bar inputs, strategy decisions, and fills."""
    import asterion_engine

    data = lab_data.Data(dataset, config.begin_day, config.end_day, version=config.dataset_version)
    contracts, series, first = lab_data.prepare(data, config)
    for contract in contracts:
        contract["bars"] = [
            bar for bar in contract["bars"]
            if (begin_ms is None or bar[1] // 10**6 >= begin_ms)
            and (end_ms is None or bar[1] // 10**6 <= end_ms)
        ]
    engine = asterion_engine.Replay(
        lab_data.raw(config.deposit),
        contracts,
        series,
        (
            lab_data.raw(config.max_order_lots),
            lab_data.raw(config.max_gross_lots),
            config.max_working_orders,
            lab_data.raw(config.max_contract_lots),
        ),
    )
    symbols = {engine.unit(index): symbol for symbol, index in first.items()}
    units = {symbol: unit for unit, symbol in symbols.items()}
    context = Context(
        [symbols[unit] for unit in sorted(symbols)],
        backtest.rolls(contracts),
        max_history=getattr(strategy, "history_limit", None),
    )

    replayed_bars = {}
    decisions = {}
    prev_contracts = {}

    while not engine.finished:
        while not engine.day_done:
            replayed = engine.next()
            bars = backtest.visible(replayed, symbols, contracts)
            if not bars:
                continue
            raw_by_symbol = {}
            end_ms = None
            for _, _, unit, leading, day, time_ns, *prices in replayed:
                if leading:
                    end_ms = time_ns // 10 ** 6
                    raw_by_symbol[symbols[unit]] = (
                        day,
                        *(Decimal(value).scaleb(-8) for value in prices),
                    )
            for bar in bars:
                day, open_, high, low, close, volume = raw_by_symbol[bar.symbol]
                rolled = bar.symbol in prev_contracts and prev_contracts[bar.symbol] != bar.contract
                roll_ratio = parse_decimal(context.rolls.get(bar.contract)) if rolled else None
                prev_contracts[bar.symbol] = bar.contract
                replayed_bars[(end_ms, bar.symbol)] = {
                    "end_ms": end_ms,
                    "symbol": bar.symbol,
                    "contract": bar.contract,
                    "trading_day": day,
                    "open": open_,
                    "high": high,
                    "low": low,
                    "close": close,
                    "volume": volume,
                    "open_interest": parse_decimal(bar.open_interest),
                    "roll_ratio": roll_ratio,
                    "roll_yield": parse_decimal(bar.roll_yield),
                }
            wanted = step(strategy, context, bars)
            targets = {}
            for symbol, val in wanted.items():
                if context.mode == "weights":
                    targets[symbol] = {"mode": "weight", "value": quantize_weight(val)}
                else:
                    targets[symbol] = {"mode": "lots", "value": Decimal(val)}
            decisions[end_ms] = {
                "end_ms": end_ms,
                "ready": context.ready,
                "mode": context.mode,
                "targets": targets,
            }
            if not context.ready or (execute_from_ms is not None and end_ms < execute_from_ms):
                for unit in symbols:
                    engine.want(unit, None)
                continue
            if context.mode == "weights" and wanted:
                sized = engine.weights(
                    {units[s]: lab_data.raw(quantize_weight(w)) for s, w in wanted.items()},
                    lab_data.raw(config.max_gross_weight),
                    lab_data.raw(config.max_net_weight),
                    lab_data.raw(config.max_margin_ratio),
                    lab_data.raw(config.min_rebalance_weight),
                )
            else:
                sized = {units[s]: lots * lab_data.R for s, lots in wanted.items()}
            for unit, quantity in sized.items():
                engine.want(unit, quantity)
            engine.decide()
        engine.settle()

    backtest.held_without_prices(engine, contracts)
    fills_table = backtest.tables(engine, contracts)["fills"]
    return {
        "data": data,
        "contracts": contracts,
        "bars": replayed_bars,
        "decisions": decisions,
        "fills": fills_table,
    }


def _percentile(values: list[float], q: float) -> float:
    if len(values) == 1:
        return values[0]
    pos = (len(values) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    return values[lo] if lo == hi else values[lo] + (values[hi] - values[lo]) * (pos - lo)


def tick_lookup(contracts: list):
    schedules = {}
    for item in contracts:
        initial = Decimal(item["price_increment"]).scaleb(-8)
        changes = [(day, Decimal(val).scaleb(-8)) for day, val in item.get("increments", ())]
        schedules[item["symbol"]] = (initial, changes)

    def lookup(contract: str, day: str) -> Decimal | None:
        entry = schedules.get(contract)
        if entry is None:
            return None
        tick, changes = entry
        for effective_day, next_tick in changes:
            if day >= effective_day:
                tick = next_tick
            else:
                break
        return tick if tick > 0 else None

    return lookup


def warehouse_contract_bars(
    data: lab_data.Data,
    canonical_contracts: set[str],
    minutes: int,
    begin_day: str,
    end_day: str,
) -> dict:
    """Load Warehouse bars for raw per-contract comparison against Trading's `bars.sqlite`."""
    result = {}
    with data.warehouse:
        for contract in sorted(canonical_contracts):
            exchange, product, _month = contract.split("/")
            if minutes == 0:
                daily = data.product(exchange, product).get(contract)
                if daily is None:
                    continue
                for day, row in sorted(daily.days.items()):
                    if not (begin_day <= day <= end_day) or not row["auction"] or row["close"] is None:
                        continue
                    end_ms = lab_data.close_ns(day) // 10 ** 6
                    result[(end_ms, contract)] = {
                        "trading_day": day,
                        "open": parse_decimal(row["open"]),
                        "high": parse_decimal(row["high"]),
                        "low": parse_decimal(row["low"]),
                        "close": parse_decimal(row["close"]),
                        "volume": parse_decimal(row["volume"]),
                        "open_interest": parse_decimal(row["open_interest"]),
                    }
            else:
                for _day, group in groupby(
                    data.minute_rows(contract, begin_day, end_day), key=itemgetter(0)
                ):
                    for bar in chart.minute_bars(list(group), minutes, contract):
                        if bar["partial"]:
                            continue
                        result[(bar["time_ms"], contract)] = {
                            "trading_day": bar["trading_day"],
                            "open": parse_decimal(bar["open"]),
                            "high": parse_decimal(bar["high"]),
                            "low": parse_decimal(bar["low"]),
                            "close": parse_decimal(bar["close"]),
                            "volume": parse_decimal(bar["volume"]),
                            "open_interest": parse_decimal(bar["open_interest"]),
                        }
    return result


def compare_decimal(
    live_val: Decimal | None,
    bt_val: Decimal | None,
    tol: Decimal,
) -> tuple[bool, Decimal | None]:
    if live_val is None and bt_val is None:
        return True, None
    if live_val is None or bt_val is None:
        return False, None
    delta = live_val - bt_val
    return abs(delta) <= tol, delta


def compare_bar_maps(
    live_bars: dict,
    bt_bars: dict,
    price_tol: Decimal,
    volume_tol: Decimal,
    roll_yield_tol: Decimal,
    include_series_fields: bool,
) -> tuple[dict, list]:
    diffs = []
    matched = missing_in_live = extra_in_live = mismatched = 0
    max_close_diff = Decimal(0)
    max_volume_diff = Decimal(0)

    for key in sorted(set(live_bars) | set(bt_bars)):
        live = live_bars.get(key)
        bt = bt_bars.get(key)
        end_ms, label = key
        if live is None:
            missing_in_live += 1
            diffs.append({
                "kind": "missing_in_live",
                "end_ms": end_ms,
                "trading_day": bt["trading_day"],
                "symbol": label,
                "backtest_close": format_decimal(bt["close"]),
            })
            continue
        if bt is None:
            extra_in_live += 1
            diffs.append({
                "kind": "extra_in_live",
                "end_ms": end_ms,
                "trading_day": live["trading_day"],
                "symbol": label,
                "live_close": format_decimal(live["close"]),
            })
            continue
        field_diffs = {}
        if include_series_fields and live.get("contract") != bt.get("contract"):
            field_diffs["contract"] = {"live": live.get("contract"), "backtest": bt.get("contract")}
        for field in BAR_PRICE_FIELDS:
            ok, delta = compare_decimal(live[field], bt[field], price_tol)
            if not ok:
                field_diffs[field] = {
                    "live": format_decimal(live[field]),
                    "backtest": format_decimal(bt[field]),
                    "delta": format_decimal(delta),
                }
            if field == "close" and delta is not None and abs(delta) > max_close_diff:
                max_close_diff = abs(delta)
        ok_vol, vol_delta = compare_decimal(live["volume"], bt["volume"], volume_tol)
        if not ok_vol:
            field_diffs["volume"] = {
                "live": format_decimal(live["volume"]),
                "backtest": format_decimal(bt["volume"]),
                "delta": format_decimal(vol_delta),
            }
        if vol_delta is not None and abs(vol_delta) > max_volume_diff:
            max_volume_diff = abs(vol_delta)
        ok_oi, oi_delta = compare_decimal(live["open_interest"], bt["open_interest"], volume_tol)
        if not ok_oi:
            field_diffs["open_interest"] = {
                "live": format_decimal(live["open_interest"]),
                "backtest": format_decimal(bt["open_interest"]),
                "delta": format_decimal(oi_delta),
            }
        if include_series_fields:
            ok_rr, rr_delta = compare_decimal(
                live.get("roll_ratio"), bt.get("roll_ratio"), roll_yield_tol
            )
            if not ok_rr:
                field_diffs["roll_ratio"] = {
                    "live": format_decimal(live.get("roll_ratio")),
                    "backtest": format_decimal(bt.get("roll_ratio")),
                    "delta": format_decimal(rr_delta),
                }
            ok_ry, ry_delta = compare_decimal(
                live.get("roll_yield"), bt.get("roll_yield"), roll_yield_tol
            )
            if not ok_ry:
                field_diffs["roll_yield"] = {
                    "live": format_decimal(live.get("roll_yield")),
                    "backtest": format_decimal(bt.get("roll_yield")),
                    "delta": format_decimal(ry_delta),
                }
        if field_diffs:
            mismatched += 1
            diffs.append({
                "kind": "field_mismatch",
                "end_ms": end_ms,
                "trading_day": live["trading_day"],
                "symbol": label,
                "fields": field_diffs,
            })
        else:
            matched += 1

    summary = {
        "matched": matched,
        "missing_in_live": missing_in_live,
        "extra_in_live": extra_in_live,
        "mismatched": mismatched,
        "max_close_diff": format_decimal(max_close_diff),
        "max_volume_diff": format_decimal(max_volume_diff),
        "ok": missing_in_live == 0 and extra_in_live == 0 and mismatched == 0,
    }
    return summary, diffs


def compare_decisions(
    live_decisions: dict,
    bt_decisions: dict,
    bar_diff_times: set[int],
    weight_tol: Decimal,
) -> tuple[dict, list]:
    diffs = []
    matched = missing_in_live = extra_in_live = mismatched = 0
    for end_ms in sorted(set(live_decisions) | set(bt_decisions)):
        live = live_decisions.get(end_ms)
        bt = bt_decisions.get(end_ms)
        if live is None:
            missing_in_live += 1
            diffs.append({
                "kind": "missing_in_live",
                "end_ms": end_ms,
                "caused_by_bar_diff": end_ms in bar_diff_times,
                "backtest_ready": bt["ready"],
            })
            continue
        if bt is None:
            extra_in_live += 1
            diffs.append({
                "kind": "extra_in_live",
                "end_ms": end_ms,
                "caused_by_bar_diff": end_ms in bar_diff_times,
                "live_ready": live["ready"],
            })
            continue
        problems = {}
        if live["ready"] != bt["ready"]:
            problems["ready"] = {"live": live["ready"], "backtest": bt["ready"]}
        target_diffs = {}
        for sym in sorted(set(live["targets"]) | set(bt["targets"])):
            lt = live["targets"].get(sym)
            bt_t = bt["targets"].get(sym)
            if lt is None or bt_t is None or lt["mode"] != bt_t["mode"]:
                target_diffs[sym] = {
                    "live": None if lt is None else f"{lt['mode']}:{format_decimal(lt['value'])}",
                    "backtest": None if bt_t is None else f"{bt_t['mode']}:{format_decimal(bt_t['value'])}",
                }
                continue
            tol = weight_tol if lt["mode"] == "weight" else Decimal(0)
            ok, delta = compare_decimal(lt["value"], bt_t["value"], tol)
            if not ok:
                target_diffs[sym] = {
                    "mode": lt["mode"],
                    "live": format_decimal(lt["value"]),
                    "backtest": format_decimal(bt_t["value"]),
                    "delta": format_decimal(delta),
                }
        if target_diffs:
            problems["targets"] = target_diffs
        if problems:
            mismatched += 1
            diffs.append({
                "kind": "decision_mismatch",
                "end_ms": end_ms,
                "sequence": live.get("sequence"),
                "caused_by_bar_diff": end_ms in bar_diff_times,
                **problems,
            })
        else:
            matched += 1
    summary = {
        "matched": matched,
        "missing_in_live": missing_in_live,
        "extra_in_live": extra_in_live,
        "mismatched": mismatched,
        "ok": missing_in_live == 0 and extra_in_live == 0 and mismatched == 0,
    }
    return summary, diffs


def aggregate_fills(rows: list) -> dict:
    """Aggregate fills by (trading_day, contract, side, offset) -> total qty, VWAP, count."""
    grouped = {}
    for row in rows:
        exchange = row["contract"].split("/", 1)[0]
        offset = row["offset"]
        if exchange not in ("SHFE", "INE") and offset in ("close_today", "close_yesterday"):
            offset = "close"
        key = (row["trading_day"], row["contract"], row["side"], offset)
        entry = grouped.setdefault(key, {"qty": Decimal(0), "notional": Decimal(0), "count": 0})
        qty, price = row["quantity"], row["price"]
        entry["qty"] += qty
        entry["notional"] += qty * price
        entry["count"] += 1
    for entry in grouped.values():
        entry["vwap"] = entry["notional"] / entry["qty"] if entry["qty"] > 0 else Decimal(0)
    return grouped


def compare_fills(
    live_fills: list,
    bt_fills: list,
    fill_price_tol: Decimal,
    *,
    tick_lookup=None,
) -> tuple[dict, list]:
    live_agg = aggregate_fills(live_fills)
    bt_agg = aggregate_fills(bt_fills)
    diffs = []
    matched = live_only = backtest_only = mismatched = 0
    max_price_diff = Decimal(0)
    adverse_prices: list[Decimal] = []
    adverse_ticks: list[float] = []

    for key in sorted(set(live_agg) | set(bt_agg)):
        day, contract, side, offset = key
        l_entry = live_agg.get(key)
        b_entry = bt_agg.get(key)
        if l_entry is None:
            backtest_only += 1
            diffs.append({
                "kind": "backtest_only",
                "trading_day": day,
                "contract": contract,
                "side": side,
                "offset": offset,
                "backtest_qty": format_decimal(b_entry["qty"]),
                "backtest_vwap": format_decimal(b_entry["vwap"]),
            })
            continue
        if b_entry is None:
            live_only += 1
            diffs.append({
                "kind": "live_only",
                "trading_day": day,
                "contract": contract,
                "side": side,
                "offset": offset,
                "live_qty": format_decimal(l_entry["qty"]),
                "live_vwap": format_decimal(l_entry["vwap"]),
            })
            continue
        qty_ok = l_entry["qty"] == b_entry["qty"]
        price_delta = l_entry["vwap"] - b_entry["vwap"]
        # Positive adverse slippage means live filled worse than backtest (buy higher / sell lower).
        adverse = price_delta if side == "buy" else -price_delta
        adverse_prices.append(adverse)
        tick = tick_lookup(contract, day) if tick_lookup is not None else None
        slip_ticks = float(adverse / tick) if tick is not None and tick > 0 else None
        if slip_ticks is not None:
            adverse_ticks.append(slip_ticks)
        if abs(price_delta) > max_price_diff:
            max_price_diff = abs(price_delta)
        price_ok = abs(price_delta) <= fill_price_tol
        if not qty_ok or not price_ok:
            mismatched += 1
            diff = {
                "kind": "fill_mismatch",
                "trading_day": day,
                "contract": contract,
                "side": side,
                "offset": offset,
                "live_qty": format_decimal(l_entry["qty"]),
                "backtest_qty": format_decimal(b_entry["qty"]),
                "qty_delta": format_decimal(l_entry["qty"] - b_entry["qty"]),
                "live_vwap": format_decimal(l_entry["vwap"]),
                "backtest_vwap": format_decimal(b_entry["vwap"]),
                "price_delta": format_decimal(price_delta),
                "adverse_price": format_decimal(adverse),
            }
            if slip_ticks is not None:
                diff["adverse_ticks"] = round(slip_ticks, 4)
            diffs.append(diff)
        else:
            matched += 1

    ordered_ticks = sorted(adverse_ticks)
    summary = {
        "live_fill_count": len(live_fills),
        "backtest_fill_count": len(bt_fills),
        "matched": matched,
        "live_only": live_only,
        "backtest_only": backtest_only,
        "mismatched": mismatched,
        "max_price_diff": format_decimal(max_price_diff),
        "mean_adverse_price": (
            format_decimal(sum(adverse_prices, Decimal(0)) / len(adverse_prices))
            if adverse_prices
            else None
        ),
        "slippage_samples": len(ordered_ticks),
        "mean_slippage_ticks": round(sum(ordered_ticks) / len(ordered_ticks), 4) if ordered_ticks else None,
        "median_slippage_ticks": round(_percentile(ordered_ticks, 0.5), 4) if ordered_ticks else None,
        "p90_slippage_ticks": round(_percentile(ordered_ticks, 0.9), 4) if ordered_ticks else None,
        "max_slippage_ticks": round(ordered_ticks[-1], 4) if ordered_ticks else None,
        "ok": live_only == 0 and backtest_only == 0 and mismatched == 0,
    }
    return summary, diffs
