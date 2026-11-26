"""Read Trading live artifacts (runs, recorded bars, journal fills) and map symbols."""

import datetime
from decimal import Decimal
import importlib
import json
import math
from pathlib import Path
import re
import sqlite3
import sys

from asterion.common_pb2 import Offset, Period, Side
from asterion.strategy_pb2 import Bar as WireBar, Bars as WireBars, Decision, Instance
from asterion.trading_pb2 import InstanceStatus
from asterion_strategy import Strategy

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

NATIVE_SYMBOL_RE = re.compile(r"^([A-Za-z]+)([0-9]{3,4})$")


def parse_decimal(value) -> Decimal | None:
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


def wire_decimal(field) -> Decimal | None:
    text = field.text.strip()
    return Decimal(text) if text else None


def format_decimal(value: Decimal | None) -> str | None:
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
            for (body,) in conn.execute(
                "SELECT body FROM records WHERE sequence > 0 ORDER BY sequence ASC"
            ):
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
                            "quantity": parse_decimal(cmd["quantity"]),
                            "price": parse_decimal(cmd["price"]),
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
                            "quantity": parse_decimal(trade["quantity"]),
                            "price": parse_decimal(trade["price"]),
                            "trade_time": trade.get("trade_time", ""),
                            "received_ms": trade.get("received_ms", 0),
                        }
    fills = sorted(
        fills_by_key.values(),
        key=lambda f: (f["trading_day"], f["received_ms"], f["trade_id"], f["side"]),
    )
    return {"orders": orders, "fills": fills}


def load_package_strategy(package_dir: Path, params: dict) -> Strategy:
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


def instance_params(instance: Instance) -> dict:
    params = {}
    for key, value in instance.params.items():
        field = value.WhichOneof("value")
        if field is not None:
            params[key] = getattr(value, field)
    return params
