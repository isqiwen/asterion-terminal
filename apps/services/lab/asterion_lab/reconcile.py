"""Live Trading <-> Lab backtest reconciliation tool.

Compares Trading's recorded `bars.sqlite`, per-instance `runs/<id>.sqlite`
inputs/decisions, and `journal.sqlite` fills against a pinned-version Lab
backtest replay, emitting a bar-by-bar discrepancy report for bars, target
positions/weights, and fills.
"""
import argparse
import datetime
from decimal import Decimal
import importlib
from itertools import groupby
import json
import math
from operator import itemgetter
import os
from pathlib import Path
import re
import sqlite3
import sys

from asterion.common_pb2 import Offset, Period, Side
from asterion.strategy_pb2 import Bar as WireBar, Bars as WireBars, Decision, Instance
from asterion.trading_pb2 import InstanceStatus
from asterion_lab import backtest, chart, repository
from asterion_lab.runs import Runs
from asterion_lab.warehouse_client import Dataset
from asterion_strategy import Context, Strategy, quantize_weight

PERIOD_MINUTES = {
    Period.PERIOD_DAY: 0,
    Period.PERIOD_MINUTES_1: 1,
    Period.PERIOD_MINUTES_5: 5,
    Period.PERIOD_MINUTES_10: 10,
    Period.PERIOD_MINUTES_15: 15,
    Period.PERIOD_MINUTES_30: 30,
    Period.PERIOD_MINUTES_60: 60,
}

SIDE_NAMES = {
    Side.SIDE_BUY: "buy",
    Side.SIDE_SELL: "sell",
}
OFFSET_NAMES = {
    Offset.OFFSET_OPEN: "open",
    Offset.OFFSET_CLOSE: "close",
    Offset.OFFSET_CLOSE_TODAY: "close_today",
    Offset.OFFSET_CLOSE_YESTERDAY: "close_yesterday",
}

BAR_PRICE_FIELDS = ("open", "high", "low", "close")
NATIVE_SYMBOL_RE = re.compile(r"^([A-Za-z]+)([0-9]{3,4})$")


def _dec(value) -> Decimal | None:
    if value is None:
        return None
    if isinstance(value, Decimal):
        return value if value.is_finite() else None
    if isinstance(value, float):
        return None if not math.isfinite(value) else Decimal(str(value))
    text = str(value).strip()
    if not text:
        return None
    parsed = Decimal(text)
    return parsed if parsed.is_finite() else None


def _wire_dec(field) -> Decimal | None:
    text = field.text.strip()
    return Decimal(text) if text else None


def _fmt(value: Decimal | None) -> str | None:
    if value is None:
        return None
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in ("", "-0") else text


def canonical_contract(exchange: str, symbol: str, reference_day: str = "") -> str:
    """Convert native CTP instrument (e.g. SHFE, rb2610) to canonical EXCHANGE/product/YYYY-MM."""
    match = NATIVE_SYMBOL_RE.fullmatch(symbol)
    if not match:
        raise ValueError(f"cannot parse native contract symbol: {exchange}/{symbol}")
    product, digits = match.group(1).lower(), match.group(2)
    if len(digits) == 4:
        year = 2000 + int(digits[:2])
        month = int(digits[2:])
    else:
        ref_year = int(reference_day[:4]) if len(reference_day) >= 4 else datetime.date.today().year
        decade = (ref_year // 10) * 10
        year = decade + int(digits[0])
        if year < ref_year - 2:
            year += 10
        elif year > ref_year + 8:
            year -= 10
        month = int(digits[1:])
    return f"{exchange}/{product}/{year:04d}-{month:02d}"


def _open_readonly(path: Path) -> sqlite3.Connection:
    if not path.is_file():
        raise FileNotFoundError(f"missing SQLite file: {path}")
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def default_instance_id(trading_account: Path) -> str:
    status_path = trading_account / "strategies" / "instance.pb"
    if not status_path.is_file():
        raise FileNotFoundError(f"no instance.pb in {trading_account / 'strategies'}; pass --instance")
    status = InstanceStatus()
    status.ParseFromString(status_path.read_bytes())
    if not status.HasField("instance") or not status.instance.id:
        raise ValueError("instance.pb contains no strategy instance; pass --instance")
    return status.instance.id


def instance_symbol_maps(instance: Instance, reference_day: str = ""):
    """Build native->(symbol, canonical_contract) and canonical symbol list for an Instance."""
    native_to_symbol = {}
    native_to_contract = {}
    symbols = []
    for inst in instance.instruments:
        native = f"{inst.exchange}/{inst.symbol}"
        contract = canonical_contract(inst.exchange, inst.symbol, reference_day)
        native_to_symbol[native] = contract
        native_to_contract[native] = contract
        symbols.append(contract)
    for series in instance.series:
        series_sym = f"{series.exchange}/{series.product.lower()}"
        symbols.append(series_sym)
        for entry in series.contracts:
            native = f"{entry.instrument.exchange}/{entry.instrument.symbol}"
            contract = f"{series_sym}/{entry.delivery_month}"
            native_to_symbol[native] = series_sym
            native_to_contract[native] = contract
    return symbols, native_to_symbol, native_to_contract


def read_live_run(trading_account: Path, instance_id: str) -> dict:
    """Read Trading's recorded instance configuration, delivered step inputs, decisions and events."""
    run_path = trading_account / "strategies" / "runs" / f"{instance_id}.sqlite"
    with _open_readonly(run_path) as conn:
        row = conn.execute("SELECT value FROM configuration").fetchone()
        if row is None:
            raise ValueError(f"strategy run has no configuration: {run_path}")
        instance = Instance()
        instance.ParseFromString(row[0])
        steps = []
        for seq, raw_input, raw_decision in conn.execute(
            "SELECT sequence, input, decision FROM steps ORDER BY sequence ASC"
        ):
            bars = WireBars()
            bars.ParseFromString(raw_input)
            decision = None
            if raw_decision is not None:
                decision = Decision()
                decision.ParseFromString(raw_decision)
            steps.append((seq, bars, decision))
        events = [
            {"time_ms": time_ms, "kind": kind, "text": text}
            for time_ms, kind, text in conn.execute(
                "SELECT time_ms, kind, event FROM events ORDER BY rowid ASC"
            )
        ]
    return {"instance": instance, "steps": steps, "events": events}


def read_recorded_bars(trading_account: Path, period: int, contracts: set[str] | None = None) -> list:
    """Read raw per-contract bars persisted in Trading's `strategies/bars.sqlite`."""
    bars_path = trading_account / "strategies" / "bars.sqlite"
    if not bars_path.is_file():
        return []
    result = []
    with _open_readonly(bars_path) as conn:
        for end_ms, exchange, symbol, blob in conn.execute(
            "SELECT end_ms, exchange, symbol, bar FROM bars WHERE period = ? "
            "ORDER BY end_ms ASC, exchange ASC, symbol ASC",
            (period,),
        ):
            native = f"{exchange}/{symbol}"
            if contracts is not None and native not in contracts:
                continue
            bar = WireBar()
            bar.ParseFromString(blob)
            result.append((end_ms, native, bar))
    return result


def read_live_fills(trading_account: Path, instance_id: str) -> dict:
    """Read strategy orders and deduplicated trades attributed to `instance_id` from journal."""
    journal_path = trading_account / "journal.sqlite"
    if not journal_path.is_file():
        return {"orders": {}, "fills": []}
    sources = []
    with _open_readonly(journal_path) as conn:
        for (name,) in conn.execute("SELECT name FROM segments ORDER BY first ASC"):
            seg_path = trading_account / "archives" / name
            if seg_path.is_file():
                sources.append(seg_path)
    sources.append(journal_path)

    orders = {}
    fills_by_key = {}
    for db_path in sources:
        with _open_readonly(db_path) as conn:
            for (body,) in conn.execute("SELECT body FROM records WHERE sequence > 0 ORDER BY sequence ASC"):
                record = json.loads(body)
                if "command" in record and "strategy" in record:
                    source = record["strategy"]
                    if source.get("instance") != instance_id:
                        continue
                    cmd = record["command"]
                    if cmd.get("action") == "submit":
                        order_id = cmd["order_id"]
                        orders[order_id] = {
                            "order_id": order_id,
                            "trading_day": record.get("trading_day", ""),
                            "venue": cmd["venue"],
                            "symbol": cmd["symbol"],
                            "side": cmd["side"],
                            "offset": cmd["offset"],
                            "quantity": _dec(cmd["quantity"]),
                            "price": _dec(cmd["price"]),
                            "target_sequence": source.get("target_sequence"),
                            "leg": source.get("leg"),
                        }
                elif "trades_recorded" in record:
                    for trade in record["trades_recorded"]:
                        order_id = trade["order_id"]
                        if order_id not in orders:
                            continue
                        dedup_key = (trade["trading_day"], trade["id"], trade["side"])
                        if dedup_key in fills_by_key:
                            continue
                        fills_by_key[dedup_key] = {
                            "trade_id": trade["id"],
                            "order_id": order_id,
                            "trading_day": trade["trading_day"],
                            "venue": trade["venue"],
                            "symbol": trade["symbol"],
                            "side": trade["side"],
                            "offset": trade["offset"],
                            "quantity": _dec(trade["quantity"]),
                            "price": _dec(trade["price"]),
                            "trade_time": trade.get("trade_time", ""),
                            "received_ms": trade.get("received_ms", 0),
                        }
    fills = sorted(
        fills_by_key.values(),
        key=lambda f: (f["trading_day"], f["received_ms"], f["trade_id"], f["side"]),
    )
    return {"orders": orders, "fills": fills}


def _load_package_strategy(package_dir: Path, params: dict) -> Strategy:
    manifest = json.loads((package_dir / "strategy.json").read_text())
    module_name, class_name = manifest["entry"].split(":")
    inserted = False
    if str(package_dir) not in sys.path:
        sys.path.insert(0, str(package_dir))
        inserted = True
    try:
        module = importlib.import_module(module_name)
        cls = getattr(module, class_name)
        return cls(**params)
    finally:
        if inserted:
            sys.path.remove(str(package_dir))


def _instance_params(instance: Instance) -> dict:
    params = {}
    for key, value in instance.params.items():
        field = value.WhichOneof("value")
        if field is not None:
            params[key] = getattr(value, field)
    return params


def _replay_backtest(
    dataset,
    config: backtest.Config,
    strategy: Strategy,
    *,
    execute_from_ms: int | None = None,
) -> dict:
    """Run Lab backtest replay and capture bar-by-bar inputs, strategy decisions, and fills."""
    import asterion_engine

    data = backtest.Data(dataset, config.begin_day, config.end_day, version=config.dataset_version)
    contracts, series, first = backtest.prepare(data, config)
    engine = asterion_engine.Replay(
        backtest.raw(config.deposit),
        contracts,
        series,
        (
            backtest.raw(config.max_order_lots),
            backtest.raw(config.max_gross_lots),
            config.max_working_orders,
            backtest.raw(config.max_contract_lots),
        ),
    )
    symbols = {engine.unit(index): symbol for symbol, index in first.items()}
    units = {symbol: unit for unit, symbol in symbols.items()}
    context = Context([symbols[unit] for unit in sorted(symbols)], backtest.rolls(contracts))

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
                roll_ratio = _dec(context.rolls.get(bar.contract)) if rolled else None
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
                    "open_interest": _dec(bar.open_interest),
                    "roll_ratio": roll_ratio,
                    "roll_yield": _dec(bar.roll_yield),
                }
            wanted = backtest.step(strategy, context, bars)
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
                    {units[s]: backtest.raw(quantize_weight(w)) for s, w in wanted.items()},
                    backtest.raw(config.max_gross_weight),
                    backtest.raw(config.max_net_weight),
                    backtest.raw(config.max_margin_ratio),
                    backtest.raw(config.min_rebalance_weight),
                )
            else:
                sized = {units[s]: lots * backtest.R for s, lots in wanted.items()}
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


def _tick_lookup(contracts: list):
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


def _warehouse_contract_bars(
    data: backtest.Data,
    canonical_contracts: set[str],
    minutes: int,
    begin_day: str,
    end_day: str,
) -> dict:
    """Load Warehouse bars for raw per-contract comparison against Trading's `bars.sqlite`."""
    result = {}
    with data.warehouse:
        for contract in sorted(canonical_contracts):
            exchange, product, month = contract.split("/")
            if minutes == 0:
                daily = data.product(exchange, product).get(contract)
                if daily is None:
                    continue
                for day, row in sorted(daily.days.items()):
                    if not (begin_day <= day <= end_day) or not row["auction"] or row["close"] is None:
                        continue
                    end_ms = backtest.close_ns(day) // 10 ** 6
                    result[(end_ms, contract)] = {
                        "trading_day": day,
                        "open": _dec(row["open"]),
                        "high": _dec(row["high"]),
                        "low": _dec(row["low"]),
                        "close": _dec(row["close"]),
                        "volume": _dec(row["volume"]),
                        "open_interest": _dec(row["open_interest"]),
                    }
            else:
                for day, group in groupby(
                    data.minute_rows(contract, begin_day, end_day), key=itemgetter(0)
                ):
                    for bar in chart.minute_bars(list(group), minutes, contract):
                        if bar["partial"]:
                            continue
                        result[(bar["time_ms"], contract)] = {
                            "trading_day": bar["trading_day"],
                            "open": _dec(bar["open"]),
                            "high": _dec(bar["high"]),
                            "low": _dec(bar["low"]),
                            "close": _dec(bar["close"]),
                            "volume": _dec(bar["volume"]),
                            "open_interest": _dec(bar["open_interest"]),
                        }
    return result


def _compare_decimal(
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


def _compare_bar_maps(
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
                "backtest_close": _fmt(bt["close"]),
            })
            continue
        if bt is None:
            extra_in_live += 1
            diffs.append({
                "kind": "extra_in_live",
                "end_ms": end_ms,
                "trading_day": live["trading_day"],
                "symbol": label,
                "live_close": _fmt(live["close"]),
            })
            continue
        field_diffs = {}
        if include_series_fields and live.get("contract") != bt.get("contract"):
            field_diffs["contract"] = {"live": live.get("contract"), "backtest": bt.get("contract")}
        for field in BAR_PRICE_FIELDS:
            ok, delta = _compare_decimal(live[field], bt[field], price_tol)
            if not ok:
                field_diffs[field] = {
                    "live": _fmt(live[field]),
                    "backtest": _fmt(bt[field]),
                    "delta": _fmt(delta),
                }
            if field == "close" and delta is not None and abs(delta) > max_close_diff:
                max_close_diff = abs(delta)
        ok_vol, vol_delta = _compare_decimal(live["volume"], bt["volume"], volume_tol)
        if not ok_vol:
            field_diffs["volume"] = {
                "live": _fmt(live["volume"]),
                "backtest": _fmt(bt["volume"]),
                "delta": _fmt(vol_delta),
            }
        if vol_delta is not None and abs(vol_delta) > max_volume_diff:
            max_volume_diff = abs(vol_delta)
        ok_oi, oi_delta = _compare_decimal(live["open_interest"], bt["open_interest"], volume_tol)
        if not ok_oi:
            field_diffs["open_interest"] = {
                "live": _fmt(live["open_interest"]),
                "backtest": _fmt(bt["open_interest"]),
                "delta": _fmt(oi_delta),
            }
        if include_series_fields:
            ok_rr, rr_delta = _compare_decimal(live.get("roll_ratio"), bt.get("roll_ratio"), roll_yield_tol)
            if not ok_rr:
                field_diffs["roll_ratio"] = {
                    "live": _fmt(live.get("roll_ratio")),
                    "backtest": _fmt(bt.get("roll_ratio")),
                    "delta": _fmt(rr_delta),
                }
            ok_ry, ry_delta = _compare_decimal(live.get("roll_yield"), bt.get("roll_yield"), roll_yield_tol)
            if not ok_ry:
                field_diffs["roll_yield"] = {
                    "live": _fmt(live.get("roll_yield")),
                    "backtest": _fmt(bt.get("roll_yield")),
                    "delta": _fmt(ry_delta),
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
        "max_close_diff": _fmt(max_close_diff),
        "max_volume_diff": _fmt(max_volume_diff),
        "ok": missing_in_live == 0 and extra_in_live == 0 and mismatched == 0,
    }
    return summary, diffs


def _compare_decisions(
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
                    "live": None if lt is None else f"{lt['mode']}:{_fmt(lt['value'])}",
                    "backtest": None if bt_t is None else f"{bt_t['mode']}:{_fmt(bt_t['value'])}",
                }
                continue
            tol = weight_tol if lt["mode"] == "weight" else Decimal(0)
            ok, delta = _compare_decimal(lt["value"], bt_t["value"], tol)
            if not ok:
                target_diffs[sym] = {
                    "mode": lt["mode"],
                    "live": _fmt(lt["value"]),
                    "backtest": _fmt(bt_t["value"]),
                    "delta": _fmt(delta),
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


def _aggregate_fills(rows: list) -> dict:
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


def _compare_fills(
    live_fills: list,
    bt_fills: list,
    fill_price_tol: Decimal,
    *,
    tick_lookup=None,
) -> tuple[dict, list]:
    live_agg = _aggregate_fills(live_fills)
    bt_agg = _aggregate_fills(bt_fills)
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
                "backtest_qty": _fmt(b_entry["qty"]),
                "backtest_vwap": _fmt(b_entry["vwap"]),
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
                "live_qty": _fmt(l_entry["qty"]),
                "live_vwap": _fmt(l_entry["vwap"]),
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
                "live_qty": _fmt(l_entry["qty"]),
                "backtest_qty": _fmt(b_entry["qty"]),
                "qty_delta": _fmt(l_entry["qty"] - b_entry["qty"]),
                "live_vwap": _fmt(l_entry["vwap"]),
                "backtest_vwap": _fmt(b_entry["vwap"]),
                "price_delta": _fmt(price_delta),
                "adverse_price": _fmt(adverse),
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
        "max_price_diff": _fmt(max_price_diff),
        "mean_adverse_price": (
            _fmt(sum(adverse_prices, Decimal(0)) / len(adverse_prices)) if adverse_prices else None
        ),
        "slippage_samples": len(ordered_ticks),
        "mean_slippage_ticks": round(sum(ordered_ticks) / len(ordered_ticks), 4) if ordered_ticks else None,
        "median_slippage_ticks": round(_percentile(ordered_ticks, 0.5), 4) if ordered_ticks else None,
        "p90_slippage_ticks": round(_percentile(ordered_ticks, 0.9), 4) if ordered_ticks else None,
        "max_slippage_ticks": round(ordered_ticks[-1], 4) if ordered_ticks else None,
        "ok": live_only == 0 and backtest_only == 0 and mismatched == 0,
    }
    return summary, diffs


def _frozen_source(lab_directory: Path, runs: Runs, run_id: str) -> Path | None:
    row = runs.get(run_id)
    prefixes = [f"run-{run_id}"]
    if row and row.get("sweep"):
        prefixes.append(f"sweep-{row['sweep']}")
    for name in prefixes:
        matches = sorted(
            path for path in (lab_directory / "sources").glob(f"{name}-*")
            if path.name.rsplit("-", 1)[0] == name
        )
        if matches:
            return matches[-1]
    return None


def reconcile_records(
    lab_directory: Path,
    *,
    instance: Instance,
    steps: list,
    recorded_raw: list,
    live_fills_raw: list,
    events: list | None = None,
    trading_account: Path | None = None,
    run_id: str = "",
    dataset_version: str = "",
    strategy_source: Path | None = None,
    fallback_source: Path | None = None,
    dataset=None,
    begin_day: str = "",
    end_day: str = "",
    deposit: Decimal = Decimal("1000000"),
    price_tol: Decimal = Decimal(0),
    volume_tol: Decimal = Decimal(0),
    roll_yield_tol: Decimal = Decimal("0.00000001"),
    weight_tol: Decimal = Decimal("0.000001"),
    fill_price_tol: Decimal = Decimal(0),
) -> dict:
    """Reconcile in-memory live strategy records against a Lab backtest replay."""
    instance_id = instance.id
    if not steps:
        raise ValueError(f"strategy instance {instance_id} recorded no steps")
    minutes = PERIOD_MINUTES.get(instance.period)
    if minutes is None:
        raise ValueError(f"unsupported strategy period: {instance.period}")

    first_step_day = steps[0][1].bars[0].trading_day
    last_step_day = steps[-1][1].bars[0].trading_day
    symbols, native_to_symbol, native_to_contract = instance_symbol_maps(instance, first_step_day)

    live_step_bars = {}
    live_decisions = {}
    for seq, wire_bars, decision in steps:
        if not wire_bars.bars:
            continue
        end_ms = wire_bars.bars[0].end_ms
        step_day = wire_bars.bars[0].trading_day
        if begin_day and step_day < begin_day:
            continue
        if end_day and step_day > end_day:
            continue
        for bar in wire_bars.bars:
            native = f"{bar.instrument.exchange}/{bar.instrument.symbol}"
            symbol = native_to_symbol.get(native) or canonical_contract(
                bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
            )
            contract = native_to_contract.get(native) or canonical_contract(
                bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
            )
            live_step_bars[(end_ms, symbol)] = {
                "end_ms": end_ms,
                "symbol": symbol,
                "contract": contract,
                "trading_day": bar.trading_day,
                "open": _wire_dec(bar.open),
                "high": _wire_dec(bar.high),
                "low": _wire_dec(bar.low),
                "close": _wire_dec(bar.close),
                "volume": Decimal(bar.volume),
                "open_interest": _wire_dec(bar.open_interest) if bar.HasField("open_interest") else None,
                "roll_ratio": _wire_dec(bar.roll_ratio) if bar.HasField("roll_ratio") else None,
                "roll_yield": _wire_dec(bar.roll_yield) if bar.HasField("roll_yield") else None,
            }
        if decision is not None:
            targets = {}
            mode = None
            for target in decision.targets:
                native = f"{target.instrument.exchange}/{target.instrument.symbol}"
                symbol = native_to_symbol.get(native) or canonical_contract(
                    target.instrument.exchange, target.instrument.symbol, step_day
                )
                size_case = target.WhichOneof("size")
                if size_case == "lots":
                    mode = "lots"
                    targets[symbol] = {"mode": "lots", "value": Decimal(target.lots)}
                elif size_case == "weight":
                    mode = "weight"
                    targets[symbol] = {"mode": "weight", "value": _wire_dec(target.weight)}
            live_decisions[end_ms] = {
                "end_ms": end_ms,
                "sequence": seq,
                "ready": decision.ready,
                "mode": mode,
                "targets": targets,
            }

    if not live_step_bars:
        raise ValueError("no live steps fall inside the requested trading day window")
    window_first_ms = min(k[0] for k in live_step_bars)
    window_last_ms = max(k[0] for k in live_step_bars)
    window_begin_day = begin_day or min(v["trading_day"] for v in live_step_bars.values())
    window_end_day = end_day or max(v["trading_day"] for v in live_step_bars.values())

    live_recorded_bars = {}
    recorded_contracts = set()
    for end_ms, native, bar in recorded_raw:
        if begin_day and bar.trading_day < begin_day:
            continue
        if end_day and bar.trading_day > end_day:
            continue
        contract = native_to_contract.get(native) or canonical_contract(
            bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
        )
        recorded_contracts.add(contract)
        live_recorded_bars[(end_ms, contract)] = {
            "trading_day": bar.trading_day,
            "open": _wire_dec(bar.open),
            "high": _wire_dec(bar.high),
            "low": _wire_dec(bar.low),
            "close": _wire_dec(bar.close),
            "volume": Decimal(bar.volume),
            "open_interest": _wire_dec(bar.open_interest) if bar.HasField("open_interest") else None,
        }

    execute_from_ms = None
    if (
        (instance.HasField("warmup") or instance.local_bars or instance.resume_from)
        and live_recorded_bars
    ):
        first_recorded_ms = min(k[0] for k in live_recorded_bars)
        if first_recorded_ms > window_first_ms:
            execute_from_ms = first_recorded_ms

    saved_fills_table = None
    if run_id:
        runs = Runs(lab_directory)
        record = runs.record(run_id)
        data_rec = record["data"]
        version_id = dataset_version or data_rec["dataset_version"]
        run_begin = data_rec["begin_day"]
        run_end = data_rec["end_day"]
        same_window = (
            run_begin <= first_step_day
            and last_step_day <= run_end
            and version_id == data_rec["dataset_version"]
        )
        run_symbols = tuple(data_rec["symbols"])
        run_period = data_rec["bars"]["period"]
        run_minutes = 0 if run_period == "day" else int(run_period[:-1])
        if run_minutes != minutes:
            raise ValueError(f"period mismatch: live={minutes}m vs backtest run={run_period}")
        risk = record["risk"]
        costs = record["costs"]
        config = backtest.Config(
            repository=record["strategy"]["repository"],
            strategy=record["strategy"]["id"],
            params=record["params"],
            symbols=run_symbols,
            begin_day=run_begin if same_window else first_step_day,
            end_day=run_end if same_window else last_step_day,
            deposit=Decimal(costs["deposit"]),
            costs=None,
            slippage_ticks=int(costs["slippage_ticks"]),
            max_order_lots=Decimal(risk["max_order_lots"]),
            max_gross_lots=Decimal(risk["max_gross_lots"]),
            max_contract_lots=Decimal(risk["max_contract_lots"]),
            max_working_orders=int(risk["max_working_orders"]),
            max_gross_weight=Decimal(risk.get("max_gross_weight", "0")),
            max_net_weight=Decimal(risk.get("max_net_weight", "0")),
            max_margin_ratio=Decimal(risk.get("max_margin_ratio", "0")),
            min_rebalance_weight=Decimal(risk.get("min_rebalance_weight", "0")),
            minutes=run_minutes,
            dataset_version=version_id,
        )
        if same_window and execute_from_ms is None:
            saved_fills_table = runs.table(run_id, "fills")
        source_dir = strategy_source or _frozen_source(lab_directory, runs, run_id) or fallback_source
        if source_dir is not None:
            strategy = repository.load(source_dir, config.strategy, config.params)
        elif trading_account is not None:
            pkg_dir = (
                trading_account / "strategies" / "packages"
                / instance.package_name / instance.package_version
            )
            strategy = _load_package_strategy(pkg_dir, config.params)
        else:
            raise ValueError(f"strategy source for run {run_id} is unavailable")
    else:
        version_id = dataset_version or (instance.warmup.dataset_version if instance.HasField("warmup") else "")
        if not version_id:
            raise ValueError("dataset_version is required when --run-id is omitted and instance has no warmup")
        params = _instance_params(instance)
        wl = instance.weight_limits if instance.HasField("weight_limits") else None
        source_dir = strategy_source or fallback_source
        config = backtest.Config(
            repository=str(source_dir or ""),
            strategy=instance.package_name,
            params=params,
            symbols=tuple(symbols),
            begin_day=first_step_day,
            end_day=last_step_day,
            deposit=deposit,
            costs=None,
            slippage_ticks=0,
            max_order_lots=Decimal("1000"),
            max_gross_lots=Decimal("10000"),
            max_contract_lots=Decimal("10000"),
            max_working_orders=100,
            max_gross_weight=Decimal(wl.gross.text) if wl and wl.gross.text else Decimal(0),
            max_net_weight=Decimal(wl.net.text) if wl and wl.net.text else Decimal(0),
            max_margin_ratio=Decimal(wl.max_margin_ratio.text) if wl and wl.max_margin_ratio.text else Decimal(0),
            min_rebalance_weight=(
                Decimal(wl.min_rebalance_weight.text) if wl and wl.min_rebalance_weight.text else Decimal(0)
            ),
            minutes=minutes,
            dataset_version=version_id,
        )
        if source_dir is not None:
            strategy = repository.load(source_dir, instance.package_name, params)
        elif trading_account is not None:
            pkg_dir = (
                trading_account / "strategies" / "packages"
                / instance.package_name / instance.package_version
            )
            strategy = _load_package_strategy(pkg_dir, params)
        else:
            raise ValueError("strategy repository or run_id is required")

    store = dataset if dataset is not None else Dataset(lab_directory)
    replayed = _replay_backtest(store, config, strategy, execute_from_ms=execute_from_ms)

    # Align backtest step bars & decisions to the live instance window.
    bt_step_bars = {
        key: val for key, val in replayed["bars"].items()
        if window_first_ms <= key[0] <= window_last_ms
    }
    bt_decisions = {
        end_ms: val for end_ms, val in replayed["decisions"].items()
        if window_first_ms <= end_ms <= window_last_ms
    }
    bar_summary, bar_diffs = _compare_bar_maps(
        live_step_bars, bt_step_bars, price_tol, volume_tol, roll_yield_tol,
        include_series_fields=True,
    )
    bar_diff_times = {item["end_ms"] for item in bar_diffs}
    decision_summary, decision_diffs = _compare_decisions(
        live_decisions, bt_decisions, bar_diff_times, weight_tol
    )

    # Compare raw per-contract bars in Trading's `bars.sqlite` against Warehouse.
    if live_recorded_bars:
        rec_first_ms = min(k[0] for k in live_recorded_bars)
        rec_last_ms = max(k[0] for k in live_recorded_bars)
        rec_begin_day = min(v["trading_day"] for v in live_recorded_bars.values())
        rec_end_day = max(v["trading_day"] for v in live_recorded_bars.values())
        wh_recorded = {
            key: val for key, val in _warehouse_contract_bars(
                replayed["data"], recorded_contracts, minutes, rec_begin_day, rec_end_day
            ).items()
            if rec_first_ms <= key[0] <= rec_last_ms
        }
        recorded_summary, recorded_diffs = _compare_bar_maps(
            live_recorded_bars, wh_recorded, price_tol, volume_tol, roll_yield_tol,
            include_series_fields=False,
        )
    else:
        recorded_summary = {
            "matched": 0, "missing_in_live": 0, "extra_in_live": 0, "mismatched": 0,
            "max_close_diff": "0", "max_volume_diff": "0", "ok": True,
        }
        recorded_diffs = []

    live_fills = []
    for fill in live_fills_raw:
        day = fill["trading_day"]
        if not (window_begin_day <= day <= window_end_day):
            continue
        native = f"{fill['venue']}/{fill['symbol']}"
        contract = native_to_contract.get(native) or canonical_contract(fill["venue"], fill["symbol"], day)
        live_fills.append({
            **fill,
            "contract": contract,
        })
    fills_frame = saved_fills_table if saved_fills_table is not None else replayed["fills"]
    # When warmup bars were replayed without orders, only post-warmup live days can produce live fills.
    fill_window_begin = begin_day or (
        min(v["trading_day"] for v in live_recorded_bars.values())
        if live_recorded_bars else window_begin_day
    )
    bt_fills = []
    for row in fills_frame.iter_rows(named=True):
        day = row["trading_day"].isoformat() if hasattr(row["trading_day"], "isoformat") else str(row["trading_day"])
        if not (fill_window_begin <= day <= window_end_day):
            continue
        bt_fills.append({
            "trading_day": day,
            "contract": row["contract"],
            "side": row["side"],
            "offset": row["offset"],
            "quantity": Decimal(row["quantity"]),
            "price": Decimal(row["price"]),
        })
    fill_summary, fill_diffs = _compare_fills(
        live_fills, bt_fills, fill_price_tol, tick_lookup=_tick_lookup(replayed["contracts"])
    )

    overall_ok = (
        bar_summary["ok"]
        and recorded_summary["ok"]
        and decision_summary["ok"]
        and fill_summary["ok"]
    )
    return {
        "ok": overall_ok,
        "metadata": {
            "instance_id": instance_id,
            "run_id": run_id,
            "package": f"{instance.package_name}/{instance.package_version}",
            "dataset_version": config.dataset_version,
            "period": "day" if minutes == 0 else f"{minutes}m",
            "symbols": list(config.symbols),
            "slippage_ticks": config.slippage_ticks,
            "window_begin_day": window_begin_day,
            "window_end_day": window_end_day,
            "fill_window_begin": fill_window_begin,
        },
        "summary": {
            "bars": bar_summary,
            "recorded_bars": recorded_summary,
            "decisions": decision_summary,
            "fills": fill_summary,
        },
        "bar_diffs": bar_diffs,
        "recorded_bar_diffs": recorded_diffs,
        "decision_diffs": decision_diffs,
        "fill_diffs": fill_diffs,
        "events": list(events or ()),
    }


def reconcile(
    trading_account: Path,
    lab_directory: Path,
    *,
    instance_id: str = "",
    run_id: str = "",
    dataset_version: str = "",
    strategy_source: Path | None = None,
    dataset=None,
    begin_day: str = "",
    end_day: str = "",
    deposit: Decimal = Decimal("1000000"),
    price_tol: Decimal = Decimal(0),
    volume_tol: Decimal = Decimal(0),
    roll_yield_tol: Decimal = Decimal("0.00000001"),
    weight_tol: Decimal = Decimal("0.000001"),
    fill_price_tol: Decimal = Decimal(0),
) -> dict:
    """Reconcile a Trading strategy instance against a Lab backtest on the same dataset version."""
    instance_id = instance_id or default_instance_id(trading_account)
    live_run = read_live_run(trading_account, instance_id)
    instance = live_run["instance"]
    steps = live_run["steps"]
    if not steps:
        raise ValueError(f"strategy instance {instance_id} recorded no steps")
    first_step_day = steps[0][1].bars[0].trading_day
    _, _, native_to_contract = instance_symbol_maps(instance, first_step_day)
    recorded_raw = read_recorded_bars(trading_account, instance.period, set(native_to_contract))
    journal = read_live_fills(trading_account, instance_id)
    return reconcile_records(
        lab_directory,
        instance=instance,
        steps=steps,
        recorded_raw=recorded_raw,
        live_fills_raw=journal["fills"],
        events=live_run["events"],
        trading_account=trading_account,
        run_id=run_id,
        dataset_version=dataset_version,
        strategy_source=strategy_source,
        dataset=dataset,
        begin_day=begin_day,
        end_day=end_day,
        deposit=deposit,
        price_tol=price_tol,
        volume_tol=volume_tol,
        roll_yield_tol=roll_yield_tol,
        weight_tol=weight_tol,
        fill_price_tol=fill_price_tol,
    )


def reconcile_rpc(lab_directory: Path, request, *, dataset=None) -> dict:
    """Reconcile a `lab.ReconcileLive` RPC request into a report dict."""
    if not request.HasField("instance"):
        raise ValueError("reconciliation requires a strategy instance")
    steps = [
        (
            step.sequence or step.input.sequence,
            step.input,
            step.decision if step.HasField("decision") else None,
        )
        for step in request.steps
    ]
    recorded_raw = [
        (bar.end_ms, f"{bar.instrument.exchange}/{bar.instrument.symbol}", bar)
        for bar in request.recorded_bars
    ]
    live_fills_raw = []
    for fill in request.live_fills:
        side = SIDE_NAMES.get(fill.side)
        offset = OFFSET_NAMES.get(fill.offset)
        qty = _wire_dec(fill.lots)
        price = _wire_dec(fill.price)
        if not side or not offset or qty is None or price is None:
            continue
        live_fills_raw.append({
            "trading_day": fill.trading_day,
            "venue": fill.instrument.exchange,
            "symbol": fill.instrument.symbol,
            "side": side,
            "offset": offset,
            "quantity": qty,
            "price": price,
        })
    deposit = _wire_dec(request.deposit) if request.HasField("deposit") else None
    return reconcile_records(
        lab_directory,
        instance=request.instance,
        steps=steps,
        recorded_raw=recorded_raw,
        live_fills_raw=live_fills_raw,
        run_id=request.run_id,
        dataset_version=request.dataset_version,
        fallback_source=Path(request.repository) if request.repository else None,
        dataset=dataset,
        begin_day=request.begin_day,
        end_day=request.end_day,
        deposit=deposit if deposit and deposit > 0 else Decimal("1000000"),
    )


def format_report(report: dict) -> str:
    meta = report["metadata"]
    summary = report["summary"]
    lines = [
        f"实盘 ↔ 回测对账结果: {'通过' if report['ok'] else '发现差异'}",
        f"  实例: {meta['instance_id']} ({meta['package']})"
        + (f" | 回测: {meta['run_id']}" if meta["run_id"] else ""),
        f"  数据版本: {meta['dataset_version'][:12]} | 周期: {meta['period']} | "
        f"区间: {meta['window_begin_day']} ~ {meta['window_end_day']}",
    ]
    b = summary["bars"]
    lines.append(
        f"  [策略输入 K 线] {'OK' if b['ok'] else 'DIFF'} "
        f"一致={b['matched']} 差异={b['mismatched']} 实盘缺={b['missing_in_live']} "
        f"实盘多={b['extra_in_live']} 最大收盘差={b['max_close_diff']} 最大量差={b['max_volume_diff']}"
    )
    r = summary["recorded_bars"]
    lines.append(
        f"  [bars.sqlite]   {'OK' if r['ok'] else 'DIFF'} "
        f"一致={r['matched']} 差异={r['mismatched']} 实盘缺={r['missing_in_live']} "
        f"实盘多={r['extra_in_live']} 最大收盘差={r['max_close_diff']} 最大量差={r['max_volume_diff']}"
    )
    d = summary["decisions"]
    lines.append(
        f"  [目标仓位决策]   {'OK' if d['ok'] else 'DIFF'} "
        f"一致={d['matched']} 差异={d['mismatched']} 实盘缺={d['missing_in_live']} 实盘多={d['extra_in_live']}"
    )
    f = summary["fills"]
    slip_text = (
        f" 实盘滑点均值={f['mean_slippage_ticks']}跳 P90={f['p90_slippage_ticks']}跳"
        if f.get("slippage_samples")
        else ""
    )
    lines.append(
        f"  [成交明细]       {'OK' if f['ok'] else 'DIFF'} "
        f"实盘={f['live_fill_count']}笔 回测={f['backtest_fill_count']}笔 "
        f"一致={f['matched']} 差异={f['mismatched']} 仅实盘={f['live_only']} "
        f"仅回测={f['backtest_only']} 最大均价差={f['max_price_diff']}{slip_text}"
    )
    for title, items in (
        ("策略输入 K 线差异", report["bar_diffs"]),
        ("bars.sqlite 差异", report["recorded_bar_diffs"]),
        ("目标仓位差异", report["decision_diffs"]),
        ("成交差异", report["fill_diffs"]),
    ):
        if items:
            lines.append(f"\n{title}（共 {len(items)} 条，展示前 10 条）：")
            for item in items[:10]:
                lines.append("  - " + json.dumps(item, ensure_ascii=False))
    return "\n".join(lines)


def main(argv=None):
    if argv is None and len(sys.argv) >= 3 and sys.argv[1] == "--rpc":
        from asterion.lab_pb2 import ReconcileLive, ReconcileResult

        output = os.fdopen(os.dup(1), "wb")
        os.dup2(2, 1)
        request = ReconcileLive()
        request.ParseFromString(sys.stdin.buffer.read())
        try:
            report = reconcile_rpc(Path(sys.argv[2]), request)
        except Exception as error:
            print(error, file=sys.stderr)
            sys.exit(1)
        reply = ReconcileResult(ok=report["ok"], report=json.dumps(report, ensure_ascii=False))
        output.write(reply.SerializeToString())
        output.flush()
        return

    parser = argparse.ArgumentParser(description="Reconcile Trading recorded bars/decisions/fills vs Lab backtest")
    parser.add_argument("--trading-account", required=True, type=Path, help="Trading data/<account> directory")
    parser.add_argument("--lab-dir", required=True, type=Path, help="Lab data directory (runs/ & warehouse-cache/)")
    parser.add_argument("--instance", default="", help="Strategy instance ID (defaults to instance.pb)")
    parser.add_argument("--run-id", default="", help="Existing Lab backtest run ID to compare against")
    parser.add_argument("--dataset-version", default="", help="Pinned Warehouse dataset version SHA-256")
    parser.add_argument("--strategy-source", type=Path, default=None, help="Strategy repository/snapshot directory")
    parser.add_argument("--begin-day", default="", help="Filter start trading day YYYY-MM-DD")
    parser.add_argument("--end-day", default="", help="Filter end trading day YYYY-MM-DD")
    parser.add_argument("--deposit", default="1000000", help="Deposit when replaying without --run-id")
    parser.add_argument("--price-tol", default="0", help="Allowed OHLC price delta")
    parser.add_argument("--volume-tol", default="0", help="Allowed volume/OI delta")
    parser.add_argument("--weight-tol", default="0.000001", help="Allowed target weight delta")
    parser.add_argument("--fill-price-tol", default="0", help="Allowed fill VWAP slippage delta")
    parser.add_argument("--output", type=Path, default=None, help="Write full JSON report to this path")
    args = parser.parse_args(argv)

    report = reconcile(
        args.trading_account,
        args.lab_dir,
        instance_id=args.instance,
        run_id=args.run_id,
        dataset_version=args.dataset_version,
        strategy_source=args.strategy_source,
        begin_day=args.begin_day,
        end_day=args.end_day,
        deposit=Decimal(args.deposit),
        price_tol=Decimal(args.price_tol),
        volume_tol=Decimal(args.volume_tol),
        weight_tol=Decimal(args.weight_tol),
        fill_price_tol=Decimal(args.fill_price_tol),
    )
    print(format_report(report))
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    sys.exit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
