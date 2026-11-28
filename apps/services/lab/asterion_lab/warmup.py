"""Fixed-version, digest-checked historical bar warmup builder for Trading."""
import collections
import datetime
from decimal import Decimal
import hashlib
import math
import re

from asterion.common_pb2 import Instrument, Period
from asterion.strategy_pb2 import Bar, Warmup

from asterion_lab import chart, dominant, pit

WARMUP_PERIODS = {
    Period.PERIOD_MINUTES_1: 1,
    Period.PERIOD_MINUTES_5: 5,
    Period.PERIOD_MINUTES_10: 10,
    Period.PERIOD_MINUTES_15: 15,
    Period.PERIOD_MINUTES_30: 30,
    Period.PERIOD_MINUTES_60: 60,
    Period.PERIOD_DAY: "day",
}


def _decimal(value):
    return None if value is None else {"text": format(Decimal(value).normalize(), "f")}


def ctp_instrument(exchange: str, cased_product: str, month: str):
    suffix = month[3:4] + month[5:] if exchange == "CZCE" else month[2:4] + month[5:]
    return Instrument(exchange=exchange, symbol=cased_product + suffix)


def contract_name(selection) -> str:
    series_match = re.fullmatch(r"(SHFE|INE|DCE|CZCE|CFFEX|GFEX)/([a-z]{1,8})", selection.contract)
    if series_match:
        exchange, product = series_match.groups()
        prev = ""
        for entry in selection.candidates:
            datetime.date.fromisoformat(entry.delivery_month + "-01")
            suffix = (
                entry.delivery_month[3:4] + entry.delivery_month[5:]
                if exchange == "CZCE"
                else entry.delivery_month[2:4] + entry.delivery_month[5:]
            )
            if (
                entry.instrument.exchange != exchange
                or entry.instrument.symbol.lower() != product + suffix
                or (prev and entry.delivery_month <= prev)
            ):
                raise ValueError(f"invalid dominant warmup candidate for {selection.contract}")
            prev = entry.delivery_month
        return selection.contract
    instrument = selection.instrument
    match = re.fullmatch(
        r"(SHFE|INE|DCE|CZCE|CFFEX|GFEX)/([a-z]{1,8})/([0-9]{4}-[0-9]{2})",
        selection.contract,
    )
    if not match:
        raise ValueError(f"invalid historical contract {selection.contract}")
    exchange, product, month = match.groups()
    datetime.date.fromisoformat(month + "-01")
    suffix = month[3:4] + month[5:] if exchange == "CZCE" else month[2:4] + month[5:]
    if instrument.exchange != exchange or instrument.symbol.lower() != product + suffix:
        raise ValueError(f"historical contract does not match {instrument.exchange}/{instrument.symbol}")
    return selection.contract


def update_batch_digest(digest, batch):
    digest.update(b"batch\n")
    for bar in batch.bars:
        suffix = f"|{bar.roll_ratio.text}" if bar.HasField("roll_ratio") else ""
        for field, tag in (
            ("roll_yield", "ry"),
            ("far_close", "fc"),
            ("near_return", "nr"),
            ("far_return", "fr"),
            ("receipt_volume", "rv"),
            ("member_long", "ml"),
            ("member_short", "ms"),
            ("inventory_total", "inv"),
        ):
            if bar.HasField(field):
                suffix += f"|{tag}={getattr(bar, field).text}"
        line = (
            f"{bar.trading_day}|{bar.end_ms}|"
            f"{bar.instrument.exchange}/{bar.instrument.symbol}|"
            f"{bar.open.text}|{bar.high.text}|{bar.low.text}|{bar.close.text}|"
            f"{bar.volume}|{bar.open_interest.text}{suffix}\n"
        )
        digest.update(line.encode())


def build_warmup(request, *, dataset, data_version) -> Warmup:
    if not 1 <= len(request.instruments) <= 64:
        raise ValueError("warmup requires 1 to 64 instruments")
    if not 1 <= request.max_bars <= 500:
        raise ValueError("warmup max_bars must be between 1 and 500")
    period = WARMUP_PERIODS.get(request.period)
    if period != "day" and period not in chart.MINUTES:
        raise ValueError("warmup bars are daily or of 1 to 60 minutes")
    contracts = [contract_name(selection) for selection in request.instruments]
    has_series = any(len(c.split("/")) == 2 for c in contracts)
    tables = ("daily",) if period == "day" else (("daily", "minutes") if has_series else ("minutes",))
    identity = data_version(request.dataset_version, tables)
    warehouse = dataset()
    with warehouse:
        version = warehouse.version(identity)
        grouped = collections.defaultdict(dict)
        daily_cache = {}
        delist_cache = {}
        pit_cache = {}
        lookback_days = max(request.max_bars * 2 + 120, 400)

        def exchange_pit(exchange_name):
            if exchange_name not in pit_cache:
                ex_days = sorted(version.days(exchange_name))
                if ex_days:
                    latest = datetime.date.fromisoformat(ex_days[-1])
                    begin_w = (latest - datetime.timedelta(days=lookback_days)).isoformat()
                    pit_cache[exchange_name] = pit.load_exchange_pit(
                        warehouse, version, exchange_name, begin_w, ex_days[-1]
                    )
                else:
                    pit_cache[exchange_name] = pit.EMPTY_PIT
            return pit_cache[exchange_name]

        def pit_extra(exchange_name, product_name, bar_day, bar_end_ms):
            rv, ml, ms, inv = exchange_pit(exchange_name).lookup(product_name, bar_day, bar_end_ms)
            out = {}
            for key, val in (
                ("receipt_volume", rv),
                ("member_long", ml),
                ("member_short", ms),
                ("inventory_total", inv),
            ):
                if math.isfinite(val):
                    out[key] = _decimal(dominant.rounded(Decimal(f"{val:.8f}")))
            return out

        for index, selection in enumerate(request.instruments):
            inst, contract = selection.instrument, contracts[index]
            parts = contract.split("/")
            if len(parts) == 2:
                exchange, product = parts
                if exchange not in delist_cache:
                    delist_map = {}
                    contracts_key = f"contracts/{exchange}"
                    if version.has(contracts_key):
                        table = warehouse.read(version, contracts_key)
                        for c_name, delist_date in table.select("contract", "delist_date").rows():
                            if delist_date is not None:
                                delist_map[c_name] = delist_date.isoformat()
                    delist_cache[exchange] = delist_map
                cased = product.upper() if exchange in ("CZCE", "CFFEX") else product
                if selection.candidates:
                    sample = selection.candidates[0]
                    suf = 3 if exchange == "CZCE" else 4
                    cased = sample.instrument.symbol[:-suf]
                candidate_map = {
                    f"{exchange}/{product}/{entry.delivery_month}": entry.instrument
                    for entry in selection.candidates
                }
                by_contract = collections.defaultdict(dict)
                exchange_days = sorted(version.days(exchange))
                begin_window = ""
                if exchange_days:
                    latest_day = datetime.date.fromisoformat(exchange_days[-1])
                    begin_window = (latest_day - datetime.timedelta(days=lookback_days)).isoformat()
                for key in version.keys("daily", exchange=exchange, begin=begin_window):
                    data = daily_cache.get((exchange, key))
                    if data is None:
                        data = warehouse.read(version, key)
                        daily_cache[(exchange, key)] = data
                    filtered = data.filter(data["product"] == product)
                    for c_name, day, open_, high, low, close, settle, vol, oi in (
                        filtered.sort("day")
                        .select(
                            "contract",
                            "day",
                            "open",
                            "high",
                            "low",
                            "close",
                            "settlement",
                            "volume",
                            "open_interest",
                        )
                        .rows()
                    ):
                        by_contract[c_name][day.isoformat()] = (
                            day,
                            open_,
                            high,
                            low,
                            close,
                            settle,
                            vol,
                            oi,
                        )
                months = []
                for c_name in sorted(by_contract):
                    days_map = by_contract[c_name]
                    traded_days = [
                        d
                        for d, r in sorted(days_map.items())
                        if None not in (r[1], r[2], r[3], r[4]) and r[5] is not None
                    ]
                    if not traded_days:
                        continue
                    item = dominant.Month(
                        c_name,
                        c_name.rsplit("/", 1)[1],
                        last_bar=traded_days[-1],
                        delist_date=delist_cache[exchange].get(c_name, ""),
                    )
                    for d, r in days_map.items():
                        if r[5] is not None:
                            item.settlement[d] = Decimal(r[5])
                        if r[7] is not None:
                            item.open_interest[d] = Decimal(r[7])
                    months.append(item)
                if len(months) < 2:
                    raise ValueError(f"dominant warmup for {contract} requires at least two months")
                dom_plan = dominant.plan(months, traded=True)
                day_segment = {}
                for seg_idx, seg in enumerate(dom_plan.segments):
                    end_bound = (
                        dom_plan.segments[seg_idx + 1].begin
                        if seg_idx + 1 < len(dom_plan.segments)
                        else ""
                    )
                    m_contract = months[seg.month].contract
                    for d in dom_plan.days:
                        if d >= seg.begin and (not end_bound or d < end_bound):
                            day_segment[d] = (seg_idx, m_contract)
                bars = []
                emitted_rolls = set()
                if period == "day":
                    prev_daily_close = {}
                    for c_name, days_map in by_contract.items():
                        prev_map, last_c = {}, None
                        for d_key, r in sorted(days_map.items()):
                            if None not in (r[1], r[2], r[3], r[4]):
                                c_val = Decimal(r[4])
                                if last_c is not None and last_c > 0:
                                    prev_map[d_key] = last_c
                                last_c = c_val
                        prev_daily_close[c_name] = prev_map
                    for d in sorted(day_segment):
                        seg_idx, m_contract = day_segment[d]
                        row = by_contract[m_contract].get(d)
                        if row is None or None in (row[1], row[2], row[3], row[4]):
                            continue
                        day_obj, open_, high, low, close, _, vol, oi = row
                        m_inst = candidate_map.get(m_contract) or ctp_instrument(
                            exchange, cased, m_contract.rsplit("/", 1)[1]
                        )
                        end_ms = (
                            int(datetime.datetime.combine(day_obj, datetime.time(15), chart.BEIJING).timestamp())
                            * 1000
                        )
                        extra = pit_extra(exchange, product, d, end_ms)
                        if oi is not None:
                            extra["open_interest"] = _decimal(oi)
                        if seg_idx > 0 and seg_idx not in emitted_rolls:
                            extra["roll_ratio"] = _decimal(dom_plan.ratios[seg_idx])
                            emitted_rolls.add(seg_idx)
                        near_px = Decimal(close)
                        prev_near = prev_daily_close.get(m_contract, {}).get(d)
                        if prev_near is not None and near_px > 0:
                            nr = dominant.simple_return(near_px, prev_near)
                            if nr is not None:
                                extra["near_return"] = _decimal(nr)
                        sub_idx = dom_plan.second.get(d)
                        if sub_idx is not None and near_px > 0:
                            sub_contract = months[sub_idx].contract
                            sub_row = by_contract[sub_contract].get(d)
                            if (
                                sub_row is not None
                                and None not in (sub_row[1], sub_row[2], sub_row[3], sub_row[4])
                                and Decimal(sub_row[4]) > 0
                            ):
                                sub_px = Decimal(sub_row[4])
                                extra["far_close"] = _decimal(sub_px)
                                extra["roll_yield"] = _decimal(
                                    dominant.annualized_roll_yield(
                                        near_px,
                                        sub_px,
                                        m_contract.rsplit("/", 1)[1],
                                        months[sub_idx].delivery_month,
                                    )
                                )
                                prev_sub = prev_daily_close.get(sub_contract, {}).get(d)
                                if prev_sub is not None:
                                    fr = dominant.simple_return(sub_px, prev_sub)
                                    if fr is not None:
                                        extra["far_return"] = _decimal(fr)
                        bars.append(
                            Bar(
                                instrument=m_inst,
                                trading_day=d,
                                end_ms=end_ms,
                                open=_decimal(open_),
                                high=_decimal(high),
                                low=_decimal(low),
                                close=_decimal(close),
                                volume=int(vol or 0),
                                **extra,
                            )
                        )
                else:
                    recent = []
                    for d in sorted(day_segment, reverse=True):
                        seg_idx, m_contract = day_segment[d]
                        _, prod, month = m_contract.split("/")
                        key = f"minutes/{exchange}/{d}/{prod}/{month}"
                        if key not in version.manifest["partitions"]:
                            continue
                        rows = (
                            warehouse.read(version, key)
                            .sort("start_ms")
                            .select(
                                "day",
                                "start_ms",
                                "partial",
                                "open",
                                "high",
                                "low",
                                "close",
                                "volume",
                                "open_interest",
                            )
                            .rows()
                        )
                        day_bars = chart.minute_bars(rows, period, m_contract)
                        sub_closes = {}
                        sub_idx = dom_plan.second.get(d)
                        sub_contract = months[sub_idx].contract if sub_idx is not None else ""
                        sub_month = months[sub_idx].delivery_month if sub_idx is not None else ""
                        if sub_idx is not None:
                            _, s_prod, s_month = sub_contract.split("/")
                            s_key = f"minutes/{exchange}/{d}/{s_prod}/{s_month}"
                            if version.has(s_key):
                                s_rows = (
                                    warehouse.read(version, s_key)
                                    .sort("start_ms")
                                    .select(
                                        "day",
                                        "start_ms",
                                        "partial",
                                        "open",
                                        "high",
                                        "low",
                                        "close",
                                        "volume",
                                        "open_interest",
                                    )
                                    .rows()
                                )
                                sub_closes = {
                                    sb["time_ms"]: Decimal(sb["close"])
                                    for sb in chart.minute_bars(s_rows, period, sub_contract)
                                    if not sb["partial"] and sb["close"] is not None
                                }
                        recent.extend(
                            (seg_idx, m_contract, bar, sub_closes.get(bar["time_ms"]), sub_contract, sub_month)
                            for bar in reversed(day_bars)
                            if not bar["partial"]
                        )
                        if len(recent) >= request.max_bars:
                            break
                    prev_min_close = {}
                    for seg_idx, m_contract, bar, sub_close, sub_contract, sub_month in reversed(
                        recent[: request.max_bars]
                    ):
                        m_inst = candidate_map.get(m_contract) or ctp_instrument(
                            exchange, cased, m_contract.rsplit("/", 1)[1]
                        )
                        t_day = bar["trading_day"].isoformat()
                        extra = pit_extra(exchange, product, t_day, bar["time_ms"])
                        if bar["open_interest"] is not None:
                            extra["open_interest"] = _decimal(bar["open_interest"])
                        if seg_idx > 0 and seg_idx not in emitted_rolls:
                            extra["roll_ratio"] = _decimal(dom_plan.ratios[seg_idx])
                            emitted_rolls.add(seg_idx)
                        near_px = Decimal(bar["close"])
                        prev_near = prev_min_close.get(m_contract)
                        if prev_near is not None and near_px > 0:
                            nr = dominant.simple_return(near_px, prev_near)
                            if nr is not None:
                                extra["near_return"] = _decimal(nr)
                        if near_px > 0:
                            prev_min_close[m_contract] = near_px
                        if sub_close is not None and sub_close > 0 and near_px > 0:
                            extra["far_close"] = _decimal(sub_close)
                            extra["roll_yield"] = _decimal(
                                dominant.annualized_roll_yield(
                                    near_px, sub_close, m_contract.rsplit("/", 1)[1], sub_month
                                )
                            )
                            prev_sub = prev_min_close.get(sub_contract)
                            if prev_sub is not None:
                                fr = dominant.simple_return(sub_close, prev_sub)
                                if fr is not None:
                                    extra["far_return"] = _decimal(fr)
                            prev_min_close[sub_contract] = sub_close
                        bars.append(
                            Bar(
                                instrument=m_inst,
                                trading_day=t_day,
                                end_ms=bar["time_ms"],
                                open=_decimal(bar["open"]),
                                high=_decimal(bar["high"]),
                                low=_decimal(bar["low"]),
                                close=_decimal(bar["close"]),
                                volume=int(bar["volume"]),
                                **extra,
                            )
                        )
                for bar in bars[-request.max_bars :]:
                    grouped[bar.end_ms][index] = bar
                continue
            bars = []
            fixed_exchange, fixed_product = parts[0], parts[1]
            table = "daily" if period == "day" else "minutes"
            keys = list(version.keys(table, exchange=inst.exchange, contract=contract))
            for key in reversed(keys):
                data = warehouse.read(version, key)
                data = data.filter(data["contract"] == contract)
                columns = ["open", "high", "low", "close", "volume", "open_interest"]
                recent = []
                if period == "day":
                    rows = data.sort("day").select("day", *columns).rows()
                    for day, open_, high, low, close, volume, interest in rows:
                        if None in (open_, high, low, close):
                            continue
                        end_ms = (
                            int(datetime.datetime.combine(day, datetime.time(15), chart.BEIJING).timestamp())
                            * 1000
                        )
                        extra = pit_extra(fixed_exchange, fixed_product, day.isoformat(), end_ms)
                        if interest is not None:
                            extra["open_interest"] = _decimal(interest)
                        recent.append(
                            Bar(
                                instrument=inst,
                                trading_day=day.isoformat(),
                                end_ms=end_ms,
                                open=_decimal(open_),
                                high=_decimal(high),
                                low=_decimal(low),
                                close=_decimal(close),
                                volume=int(volume or 0),
                                **extra,
                            )
                        )
                else:
                    rows = data.sort("start_ms").select("day", "start_ms", "partial", *columns).rows()
                    for bar in chart.minute_bars(rows, period, contract):
                        if bar["partial"]:
                            continue
                        t_day = bar["trading_day"].isoformat()
                        extra = pit_extra(fixed_exchange, fixed_product, t_day, bar["time_ms"])
                        if bar["open_interest"] is not None:
                            extra["open_interest"] = _decimal(bar["open_interest"])
                        recent.append(
                            Bar(
                                instrument=inst,
                                trading_day=t_day,
                                end_ms=bar["time_ms"],
                                open=_decimal(bar["open"]),
                                high=_decimal(bar["high"]),
                                low=_decimal(bar["low"]),
                                close=_decimal(bar["close"]),
                                volume=int(bar["volume"]),
                                **extra,
                            )
                        )
                bars = recent + bars
                if len(bars) >= request.max_bars:
                    break
            for bar in bars[-request.max_bars :]:
                grouped[bar.end_ms][index] = bar
    if not grouped:
        raise ValueError("no stored bars for the requested warmup instruments")
    warmup = Warmup(dataset_version=version.identity)
    digest = hashlib.sha256()
    for sequence, end_ms in enumerate(sorted(grouped)[-request.max_bars :], 1):
        batch = warmup.bars.add(sequence=sequence)
        for index in sorted(grouped[end_ms]):
            batch.bars.add().CopyFrom(grouped[end_ms][index])
        update_batch_digest(digest, batch)
    warmup.sha256 = digest.hexdigest()
    return warmup
