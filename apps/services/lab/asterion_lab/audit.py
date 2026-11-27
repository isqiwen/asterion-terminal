"""Warehouse data quality audit tool.

Scans one fixed Warehouse dataset version for:
  1. Missing trading days (daily, settlement, and minute coverage against calendar);
  2. Minute-vs-daily volume cross-checks (per contract and trading day, with source tag);
  3. OHLC sanity errors, extreme price jumps, and extreme open-interest jumps.

Supports both a direct Warehouse directory (`--warehouse-dir`, e.g. on the Linux host)
and a Lab cache directory (`--lab-dir`, optionally `--terminal-data`).
"""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import sys

import polars as pl

from asterion_lab.warehouse import Version
from asterion_lab.warehouse_client import Dataset, Endpoint


class LocalWarehouse:
    """Read-only view of an on-disk Warehouse store (e.g. `sources/research/warehouse`)."""

    def __init__(self, directory: Path, provider: str = "research"):
        direct = directory / "versions"
        if direct.is_dir():
            self.root = directory
            self.data_root = directory.parents[2] if directory.parent.name == provider and directory.parents[1].name == "sources" else directory.parent
        elif (directory / "sources" / provider / "warehouse" / "versions").is_dir():
            self.root = directory / "sources" / provider / "warehouse"
            self.data_root = directory
        elif (directory / "warehouse" / "versions").is_dir():
            self.root = directory / "warehouse"
            self.data_root = directory
        else:
            raise FileNotFoundError(f"no Warehouse store found under {directory}")

    def empty_keys(self, table: str) -> set[str]:
        path = self.data_root / "acquisition" / f"empty-{table}.txt"
        if not path.is_file():
            return set()
        return {line.strip() for line in path.read_text().splitlines() if line.strip()}

    def __enter__(self):
        return self

    def __exit__(self, *ignored):
        pass

    def current(self) -> Version:
        current_file = self.root / "current"
        if not current_file.is_file():
            raise FileNotFoundError(f"Warehouse has no published version: {current_file}")
        return self.version(current_file.read_text().strip())

    def version(self, identity: str) -> Version:
        path = self.root / "versions" / f"{identity}.json"
        if not path.is_file():
            raise FileNotFoundError(f"no Warehouse version {identity}: {path}")
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != identity:
            raise ValueError(f"Warehouse version {identity} digest mismatch")
        manifest = json.loads(raw)
        partitions = {}
        for shard_digest in manifest.get("shards", {}).values():
            shard_path = self.root / "shards" / f"{shard_digest}.json"
            shard_raw = shard_path.read_bytes()
            if hashlib.sha256(shard_raw).hexdigest() != shard_digest:
                raise ValueError(f"Warehouse shard {shard_digest} digest mismatch")
            partitions.update(json.loads(shard_raw))
        manifest = {k: v for k, v in manifest.items() if k != "shards"}
        manifest["partitions"] = partitions
        return Version(identity, manifest)

    def read(self, version: Version, key: str) -> pl.DataFrame:
        part = version.manifest["partitions"][key]
        return pl.read_parquet(self.root / part["file"])


def _dec(value) -> Decimal | None:
    if value is None:
        return None
    d = Decimal(str(value))
    return d if d.is_finite() else None


def _fmt(value: Decimal | None) -> str | None:
    if value is None:
        return None
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in ("", "-0") else text


def _iso(day) -> str:
    return day.isoformat() if hasattr(day, "isoformat") else str(day)


def _in_range(day: str, begin_day: str, end_day: str) -> bool:
    if begin_day and day < begin_day:
        return False
    if end_day and day > end_day:
        return False
    return True


def audit(
    store,
    *,
    version_id: str = "",
    exchanges: set[str] | None = None,
    begin_day: str = "",
    end_day: str = "",
    volume_tol: Decimal = Decimal(0),
    volume_rel_tol: Decimal = Decimal(0),
    max_price_jump: Decimal = Decimal("0.25"),
    max_minute_jump: Decimal = Decimal("0.15"),
    oi_jump_ratio: Decimal = Decimal("5"),
    min_oi: Decimal = Decimal("100"),
) -> dict:
    """Run all data-quality checks on one fixed Warehouse version."""
    with store:
        version = store.version(version_id) if version_id else store.current()
        partitions = version.manifest["partitions"]

        all_exchanges = sorted({
            key.split("/")[1]
            for key in partitions
            if len(key.split("/")) >= 2 and key.split("/")[0] in ("calendar", "daily", "minutes", "settlement")
        })
        selected_exchanges = (
            [ex for ex in all_exchanges if ex in exchanges] if exchanges else all_exchanges
        )

        # 1. Load calendars and contract listed lives.
        calendars = {}  # exchange -> {day: open_bool}
        for ex in selected_exchanges:
            cal_map = {}
            for key in version.keys("calendar", exchange=ex):
                df = store.read(version, key)
                for day, is_open in df.select("day", "open").iter_rows():
                    cal_map[_iso(day)] = bool(is_open)
            calendars[ex] = cal_map

        contract_lives = {}  # contract -> (list_day, delist_day)
        for ex in selected_exchanges:
            for key in version.keys("contracts", exchange=ex):
                df = store.read(version, key)
                for contract, listed, delisted in df.select("contract", "list_date", "delist_date").iter_rows():
                    if listed is not None and delisted is not None:
                        contract_lives[contract] = (_iso(listed), _iso(delisted))

        # 2. Scan missing trading days in daily and settlement partitions, and load daily frames.
        empty_daily = store.empty_keys("daily") if hasattr(store, "empty_keys") else set()
        empty_settlement = store.empty_keys("settlement") if hasattr(store, "empty_keys") else set()
        empty_minutes = store.empty_keys("minutes") if hasattr(store, "empty_keys") else set()
        source_empty_skipped = 0
        missing_daily = []
        missing_settlement = []
        daily_frames: list[pl.DataFrame] = []

        for ex in selected_exchanges:
            daily_keys = list(version.keys("daily", exchange=ex, begin=begin_day, end=end_day))
            stored_daily_days = set()
            for key in daily_keys:
                part = partitions[key]
                stored_daily_days.update(part.get("days", []))
                df = store.read(version, key).select(
                    "contract",
                    pl.col("day").cast(pl.String).alias("day"),
                    "open",
                    "high",
                    "low",
                    "close",
                    "settlement",
                    "volume",
                    "open_interest",
                    pl.lit(part.get("source", "")).alias("_source"),
                )
                if not df.is_empty():
                    stored_daily_days.update(df["day"].unique().to_list())
                    if begin_day:
                        df = df.filter(pl.col("day") >= pl.lit(begin_day))
                    if end_day:
                        df = df.filter(pl.col("day") <= pl.lit(end_day))
                    if not df.is_empty():
                        daily_frames.append(df)

            open_days = sorted(
                d for d, is_open in calendars.get(ex, {}).items()
                if is_open and _in_range(d, begin_day, end_day)
            )
            if open_days and stored_daily_days:
                lo = begin_day or min(stored_daily_days)
                hi = end_day or max(stored_daily_days)
                expected = [d for d in open_days if lo <= d <= hi]
                for d in expected:
                    if d not in stored_daily_days:
                        if f"daily/{ex}/{d}" in empty_daily:
                            source_empty_skipped += 1
                            continue
                        missing_daily.append({"exchange": ex, "day": d})

            settle_keys = list(version.keys("settlement", exchange=ex, begin=begin_day, end=end_day))
            if open_days and settle_keys:
                stored_settle_days = set()
                for key in settle_keys:
                    stored_settle_days.update(partitions[key].get("days", []))
                    parts = key.split("/")
                    if len(parts) >= 3 and len(parts[2]) == 10:
                        stored_settle_days.add(parts[2])
                if stored_settle_days:
                    lo = begin_day or min(stored_settle_days)
                    hi = end_day or max(stored_settle_days)
                    for d in open_days:
                        if lo <= d <= hi and d not in stored_settle_days:
                            if f"settlement/{ex}/{d}" in empty_settlement:
                                source_empty_skipped += 1
                                continue
                            missing_settlement.append({"exchange": ex, "day": d})

        # 3. Vectorized daily price & open-interest anomaly scan in Polars.
        open_index_by_ex = {
            ex: {d: idx for idx, d in enumerate(sorted(day for day, is_open in cal.items() if is_open))}
            for ex, cal in calendars.items()
        }
        price_anomalies = []
        oi_anomalies = []
        daily_lookup: dict[tuple[str, str], tuple[Decimal | None, bool, str]] = {}
        daily_rows_checked = 0

        if daily_frames:
            daily_df = (
                pl.concat(daily_frames, rechunk=False)
                .unique(subset=["contract", "day"], keep="last")
                .sort(["contract", "day"])
            )
            daily_rows_checked = daily_df.height
            # Map (exchange, day) -> trading-day ordinal and contract -> (list_idx, delist_day, delist_idx).
            open_idx_rows = [
                (ex, d, idx) for ex, mapping in open_index_by_ex.items() for d, idx in mapping.items()
            ]
            open_idx_df = (
                pl.DataFrame(
                    open_idx_rows,
                    schema={"_ex": pl.String, "day": pl.String, "_day_idx": pl.Int64},
                    orient="row",
                )
                if open_idx_rows
                else pl.DataFrame(schema={"_ex": pl.String, "day": pl.String, "_day_idx": pl.Int64})
            )
            life_rows = []
            for c_name, (l_day, d_day) in contract_lives.items():
                c_ex = c_name.split("/")[0]
                oi_map = open_index_by_ex.get(c_ex, {})
                life_rows.append((c_name, oi_map.get(l_day), d_day, oi_map.get(d_day)))
            life_df = (
                pl.DataFrame(
                    life_rows,
                    schema={
                        "contract": pl.String,
                        "_list_idx": pl.Int64,
                        "_delist_day": pl.String,
                        "_delist_idx": pl.Int64,
                    },
                    orient="row",
                )
                if life_rows
                else pl.DataFrame(
                    schema={
                        "contract": pl.String,
                        "_list_idx": pl.Int64,
                        "_delist_day": pl.String,
                        "_delist_idx": pl.Int64,
                    }
                )
            )
            eps = 1e-9
            max_pj_f = float(max_price_jump)
            oi_ratio_f = float(oi_jump_ratio)
            min_oi_f = float(min_oi)

            o_f = pl.col("open").cast(pl.Float64)
            h_f = pl.col("high").cast(pl.Float64)
            lo_f = pl.col("low").cast(pl.Float64)
            c_f = pl.col("close").cast(pl.Float64)
            s_f = pl.col("settlement").cast(pl.Float64)
            v_f = pl.col("volume").cast(pl.Float64)
            oi_f = pl.col("open_interest").cast(pl.Float64)

            has_ohlc = o_f.is_not_null() & h_f.is_not_null() & lo_f.is_not_null() & c_f.is_not_null()
            enriched = (
                daily_df.with_columns(pl.col("contract").str.split_exact("/", 1).struct.field("field_0").alias("_ex"))
                .join(open_idx_df, on=["_ex", "day"], how="left")
                .join(life_df, on="contract", how="left")
                .with_columns(
                    pl.when(c_f > 0).then(pl.col("close")).forward_fill().shift(1).over("contract").alias("_prev_close"),
                    pl.when(c_f > 0).then(pl.col("_day_idx")).forward_fill().shift(1).over("contract").alias("_prev_close_idx"),
                    pl.col("open_interest").forward_fill().shift(1).over("contract").alias("_prev_oi"),
                )
            )
            prev_c_f = pl.col("_prev_close").cast(pl.Float64)
            prev_oi_f = pl.col("_prev_oi").cast(pl.Float64)
            adjacent = (
                pl.col("_day_idx").is_null()
                | pl.col("_prev_close_idx").is_null()
                | ((pl.col("_day_idx") - pl.col("_prev_close_idx")) <= 1)
            )
            near_edge = (
                (pl.col("_day_idx").is_not_null() & pl.col("_list_idx").is_not_null() & ((pl.col("_day_idx") - pl.col("_list_idx")) < 5))
                | (pl.col("_day_idx").is_not_null() & pl.col("_delist_idx").is_not_null() & ((pl.col("_delist_idx") - pl.col("_day_idx")) <= 5))
            )
            flag_ohlc = has_ohlc & (
                (h_f < pl.max_horizontal(o_f, c_f, lo_f))
                | (lo_f > pl.min_horizontal(o_f, c_f, h_f))
                | (c_f <= 0)
                | (o_f <= 0)
                | (lo_f <= 0)
            )
            flag_missing_ohlc = (~has_ohlc) & v_f.is_not_null() & (v_f > 0)
            flag_non_pos_settle = s_f.is_not_null() & (s_f <= 0)
            flag_neg_vol = v_f.is_not_null() & (v_f < 0)
            flag_cs_div = (c_f > 0) & (s_f > 0) & (((c_f / s_f) - 1.0).abs() > max_pj_f - eps)
            flag_p_jump = (c_f > 0) & (prev_c_f > 0) & adjacent & (((c_f / prev_c_f) - 1.0).abs() > max_pj_f - eps)
            flag_neg_oi = oi_f.is_not_null() & (oi_f < 0)
            delist_day_col = pl.col("_delist_day").fill_null("")
            flag_oi_drop = (
                (oi_f == 0)
                & (prev_oi_f >= min_oi_f - eps)
                & ((delist_day_col == "") | (pl.col("day") < delist_day_col))
            )
            flag_oi_jump = (
                (oi_f > 0)
                & (prev_oi_f >= min_oi_f - eps)
                & (~near_edge)
                & (((oi_f / prev_oi_f) >= oi_ratio_f - eps) | ((prev_oi_f / oi_f) >= oi_ratio_f - eps))
            )

            flagged = enriched.filter(
                flag_ohlc.fill_null(False)
                | flag_missing_ohlc.fill_null(False)
                | flag_non_pos_settle.fill_null(False)
                | flag_neg_vol.fill_null(False)
                | flag_cs_div.fill_null(False)
                | flag_p_jump.fill_null(False)
                | flag_neg_oi.fill_null(False)
                | flag_oi_drop.fill_null(False)
                | flag_oi_jump.fill_null(False)
            )
            for row in flagged.select(
                "contract", "day", "open", "high", "low", "close", "settlement",
                "volume", "open_interest", "_source", "_prev_close", "_prev_close_idx",
                "_day_idx", "_prev_oi", "_list_idx", "_delist_day", "_delist_idx",
            ).iter_rows():
                (
                    contract, day, open_, high, low, close, settle, vol, oi,
                    source, prev_close, prev_close_idx, day_idx, prev_oi,
                    list_idx, delist_day, delist_idx,
                ) = row
                open_, high, low, close = _dec(open_), _dec(high), _dec(low), _dec(close)
                settle, vol, oi = _dec(settle), _dec(vol), _dec(oi)
                prev_close, prev_oi = _dec(prev_close), _dec(prev_oi)
                delist_day = delist_day or ""
                if None not in (open_, high, low, close):
                    if high < max(open_, close, low) or low > min(open_, close, high) or close <= 0 or open_ <= 0 or low <= 0:
                        price_anomalies.append({
                            "kind": "ohlc_invalid", "table": "daily",
                            "contract": contract, "day": day, "source": source,
                            "open": _fmt(open_), "high": _fmt(high), "low": _fmt(low), "close": _fmt(close),
                        })
                elif vol is not None and vol > 0:
                    price_anomalies.append({
                        "kind": "missing_ohlc_with_volume", "table": "daily",
                        "contract": contract, "day": day, "source": source,
                        "volume": _fmt(vol),
                    })
                if settle is not None and settle <= 0:
                    price_anomalies.append({
                        "kind": "non_positive_settlement", "table": "daily",
                        "contract": contract, "day": day, "source": source,
                        "settlement": _fmt(settle),
                    })
                if vol is not None and vol < 0:
                    price_anomalies.append({
                        "kind": "negative_volume", "table": "daily",
                        "contract": contract, "day": day, "source": source,
                        "volume": _fmt(vol),
                    })
                if close is not None and close > 0 and settle is not None and settle > 0:
                    cs_diff = abs(close / settle - Decimal(1))
                    if cs_diff > max_price_jump:
                        price_anomalies.append({
                            "kind": "close_settlement_divergence", "table": "daily",
                            "contract": contract, "day": day, "source": source,
                            "close": _fmt(close), "settlement": _fmt(settle),
                            "ratio_diff": _fmt(cs_diff.quantize(Decimal("0.0001"))),
                        })
                if close is not None and close > 0 and prev_close is not None and prev_close > 0:
                    is_adj = day_idx is None or prev_close_idx is None or day_idx - prev_close_idx <= 1
                    if is_adj:
                        jump = abs(close / prev_close - Decimal(1))
                        if jump > max_price_jump:
                            price_anomalies.append({
                                "kind": "price_jump", "table": "daily",
                                "contract": contract, "day": day, "source": source,
                                "prev_close": _fmt(prev_close), "close": _fmt(close),
                                "jump": _fmt(jump.quantize(Decimal("0.0001"))),
                            })
                if oi is not None:
                    if oi < 0:
                        oi_anomalies.append({
                            "kind": "negative_open_interest", "contract": contract,
                            "day": day, "open_interest": _fmt(oi),
                        })
                    elif prev_oi is not None and prev_oi >= min_oi:
                        is_near_edge = (
                            (day_idx is not None and list_idx is not None and day_idx - list_idx < 5)
                            or (day_idx is not None and delist_idx is not None and delist_idx - day_idx <= 5)
                        )
                        if oi == 0 and (not delist_day or day < delist_day):
                            oi_anomalies.append({
                                "kind": "open_interest_drop_to_zero", "contract": contract,
                                "day": day, "prev_open_interest": _fmt(prev_oi), "open_interest": "0",
                            })
                        elif not is_near_edge and oi > 0 and (oi / prev_oi >= oi_jump_ratio or prev_oi / oi >= oi_jump_ratio):
                            oi_anomalies.append({
                                "kind": "open_interest_jump", "contract": contract,
                                "day": day, "prev_open_interest": _fmt(prev_oi),
                                "open_interest": _fmt(oi),
                            })

            # Compact lookup for minute-vs-daily cross-check (only needed when minutes exist).
            has_minutes = any(
                any(version.keys("minutes", exchange=ex, begin=begin_day, end=end_day))
                for ex in selected_exchanges
            )
            if has_minutes:
                for contract, day, vol, has_settle, source in daily_df.select(
                    "contract", "day", "volume", pl.col("settlement").is_not_null(), "_source"
                ).iter_rows():
                    daily_lookup[(contract, day)] = (vol, bool(has_settle), source)

        # 4. Minute coverage, minute-vs-daily volume cross-check, and vectorized intraday minute anomalies.
        minute_days_by_contract = {}  # contract -> set[day]
        volume_mismatches = []
        checked_minute_partitions = 0
        max_mj_f = float(max_minute_jump)
        eps_m = 1e-9

        for ex in selected_exchanges:
            for key in version.keys("minutes", exchange=ex, begin=begin_day, end=end_day):
                parts = key.split("/")
                if len(parts) != 5:
                    continue
                _, exchange, day, product, month = parts
                if not _in_range(day, begin_day, end_day):
                    continue
                contract = f"{exchange}/{product}/{month}"
                minute_days_by_contract.setdefault(contract, set()).add(day)
                checked_minute_partitions += 1
                part = partitions[key]
                source = part.get("source", "")
                df = store.read(version, key).select(
                    "start_ms", "partial", "open", "high", "low", "close", "volume"
                ).sort("start_ms")

                if df.is_empty():
                    min_vol = Decimal(0)
                    missing_vol_rows = 0
                    partial_rows = 0
                else:
                    mo_f = pl.col("open").cast(pl.Float64)
                    mh_f = pl.col("high").cast(pl.Float64)
                    ml_f = pl.col("low").cast(pl.Float64)
                    mc_f = pl.col("close").cast(pl.Float64)
                    mv_f = pl.col("volume").cast(pl.Float64)
                    m_has_ohlc = mo_f.is_not_null() & mh_f.is_not_null() & ml_f.is_not_null() & mc_f.is_not_null()
                    prev_mc = pl.when(mc_f > 0).then(pl.col("close")).forward_fill().shift(1)
                    prev_mc_f = prev_mc.cast(pl.Float64)
                    m_bad_vol = mv_f.is_not_null() & (mv_f < 0)
                    m_bad_ohlc = m_has_ohlc & (
                        (mh_f < pl.max_horizontal(mo_f, mc_f, ml_f))
                        | (ml_f > pl.min_horizontal(mo_f, mc_f, mh_f))
                        | (mc_f <= 0)
                        | (ml_f <= 0)
                    )
                    m_bad_jump = (
                        m_has_ohlc
                        & (mc_f > 0)
                        & (prev_mc_f > 0)
                        & (((mc_f / prev_mc_f) - 1.0).abs() > max_mj_f - eps_m)
                    )
                    stats = df.select(
                        pl.col("volume").sum().alias("min_vol"),
                        pl.col("volume").is_null().sum().alias("missing_vol"),
                        pl.col("partial").fill_null(False).sum().alias("partial_rows"),
                        (
                            m_bad_vol.fill_null(False)
                            | m_bad_ohlc.fill_null(False)
                            | m_bad_jump.fill_null(False)
                        ).any().alias("has_anom"),
                    ).row(0)
                    min_vol = _dec(stats[0]) or Decimal(0)
                    missing_vol_rows = int(stats[1])
                    partial_rows = int(stats[2])
                    if stats[3]:
                        anom_df = (
                            df.with_columns(prev_mc.alias("_prev_close"))
                            .filter(
                                m_bad_vol.fill_null(False)
                                | m_bad_ohlc.fill_null(False)
                                | m_bad_jump.fill_null(False)
                            )
                        )
                        for start_ms, _p, open_, high, low, close, vol, prev_m_close in anom_df.iter_rows():
                            o_d, h_d, l_d, c_d, v_d = _dec(open_), _dec(high), _dec(low), _dec(close), _dec(vol)
                            prev_d = _dec(prev_m_close)
                            if v_d is not None and v_d < 0:
                                price_anomalies.append({
                                    "kind": "negative_volume", "table": "minutes",
                                    "contract": contract, "day": day, "start_ms": start_ms,
                                    "source": source, "volume": _fmt(v_d),
                                })
                            if None not in (o_d, h_d, l_d, c_d):
                                if h_d < max(o_d, c_d, l_d) or l_d > min(o_d, c_d, h_d) or c_d <= 0 or l_d <= 0:
                                    price_anomalies.append({
                                        "kind": "ohlc_invalid", "table": "minutes",
                                        "contract": contract, "day": day, "start_ms": start_ms,
                                        "source": source,
                                        "open": _fmt(o_d), "high": _fmt(h_d), "low": _fmt(l_d), "close": _fmt(c_d),
                                    })
                                if prev_d is not None and prev_d > 0 and c_d > 0:
                                    jump = abs(c_d / prev_d - Decimal(1))
                                    if jump > max_minute_jump:
                                        price_anomalies.append({
                                            "kind": "minute_price_jump", "table": "minutes",
                                            "contract": contract, "day": day, "start_ms": start_ms,
                                            "source": source,
                                            "prev_close": _fmt(prev_d), "close": _fmt(c_d),
                                            "jump": _fmt(jump.quantize(Decimal("0.0001"))),
                                        })

                daily_entry = daily_lookup.get((contract, day))
                if daily_entry is None or daily_entry[0] is None:
                    volume_mismatches.append({
                        "kind": "missing_daily_for_minutes",
                        "contract": contract,
                        "day": day,
                        "minute_volume": _fmt(min_vol),
                        "daily_volume": None,
                        "minute_rows": df.height,
                        "partial_minutes": partial_rows,
                        "source": source,
                    })
                    continue
                d_vol, _has_settle, daily_source = daily_entry
                diff = min_vol - d_vol
                rel = abs(diff) / d_vol if d_vol > 0 else (Decimal(0) if diff == 0 else Decimal(1))
                if missing_vol_rows > 0 or (abs(diff) > volume_tol and rel > volume_rel_tol):
                    volume_mismatches.append({
                        "kind": "volume_mismatch",
                        "contract": contract,
                        "day": day,
                        "minute_volume": _fmt(min_vol),
                        "daily_volume": _fmt(d_vol),
                        "diff": _fmt(diff),
                        "relative_diff": _fmt(rel.quantize(Decimal("0.0001"))),
                        "minute_rows": df.height,
                        "partial_minutes": partial_rows,
                        "missing_volume_rows": missing_vol_rows,
                        "minute_source": source,
                        "daily_source": daily_source,
                    })

        # Missing minute trading days within each contract's stored minute span.
        missing_minutes = []
        for contract, stored_days in sorted(minute_days_by_contract.items()):
            ex = contract.split("/")[0]
            open_days = calendars.get(ex, {})
            if not open_days or not stored_days:
                continue
            lo = begin_day or min(stored_days)
            hi = end_day or max(stored_days)
            if contract in contract_lives:
                listed, delisted = contract_lives[contract]
                lo = max(lo, listed)
                hi = min(hi, delisted)
            for day, is_open in sorted(open_days.items()):
                if not is_open or not (lo <= day <= hi) or day in stored_days:
                    continue
                _, product, month = contract.split("/")
                if f"minutes/{ex}/{day}/{product}/{month}" in empty_minutes:
                    source_empty_skipped += 1
                    continue
                daily_entry = daily_lookup.get((contract, day))
                if daily_entry is not None and daily_entry[0] == 0 and daily_entry[1]:
                    continue
                missing_minutes.append({"contract": contract, "day": day})

        ok = (
            not missing_daily
            and not missing_settlement
            and not missing_minutes
            and not volume_mismatches
            and not price_anomalies
            and not oi_anomalies
        )
        return {
            "ok": ok,
            "dataset_version": version.identity,
            "exchanges": selected_exchanges,
            "begin_day": begin_day,
            "end_day": end_day,
            "summary": {
                "daily_rows_checked": daily_rows_checked,
                "minute_partitions_checked": checked_minute_partitions,
                "missing_daily_days": len(missing_daily),
                "missing_settlement_days": len(missing_settlement),
                "missing_minute_days": len(missing_minutes),
                "source_empty_skipped": source_empty_skipped,
                "volume_mismatches": len(volume_mismatches),
                "price_anomalies": len(price_anomalies),
                "open_interest_anomalies": len(oi_anomalies),
            },
            "missing_daily_days": missing_daily,
            "missing_settlement_days": missing_settlement,
            "missing_minute_days": missing_minutes,
            "volume_mismatches": volume_mismatches,
            "price_anomalies": price_anomalies,
            "open_interest_anomalies": oi_anomalies,
        }


def format_report(report: dict) -> str:
    s = report["summary"]
    lines = [
        f"Warehouse 数据质量自检: {'通过' if report['ok'] else '发现异常'}",
        f"  数据版本: {report['dataset_version'][:12]} | 交易所: {', '.join(report['exchanges']) or '无'}",
        f"  扫描范围: 日线 {s['daily_rows_checked']} 行, 分钟分区 {s['minute_partitions_checked']} 个",
        f"  [缺失交易日] 日线={s['missing_daily_days']} 结算={s['missing_settlement_days']} 分钟={s['missing_minute_days']}",
        f"  [分钟/日线成交量] 差异={s['volume_mismatches']}",
        f"  [价格/持仓异常] 价格={s['price_anomalies']} 持仓={s['open_interest_anomalies']}",
    ]
    for title, key in (
        ("缺失日线交易日", "missing_daily_days"),
        ("缺失结算交易日", "missing_settlement_days"),
        ("缺失分钟交易日", "missing_minute_days"),
        ("分钟/日线成交量差异", "volume_mismatches"),
        ("异常价格", "price_anomalies"),
        ("异常持仓跳变", "open_interest_anomalies"),
    ):
        items = report[key]
        if items:
            lines.append(f"\n{title}（共 {len(items)} 条，展示前 10 条）：")
            for item in items[:10]:
                lines.append("  - " + json.dumps(item, ensure_ascii=False))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Audit Warehouse dataset quality on a fixed version")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--warehouse-dir", type=Path, help="Local Warehouse data directory or store path")
    group.add_argument("--lab-dir", type=Path, help="Lab cache directory (warehouse-cache/research/)")
    parser.add_argument("--terminal-data", type=Path, default=None, help="Terminal data dir for remote fetch")
    parser.add_argument("--provider", default="research", help="Warehouse provider store (default: research)")
    parser.add_argument("--version", default="", help="Fixed dataset version SHA-256 (default: current)")
    parser.add_argument("--exchange", action="append", default=[], help="Filter to exchange(s), repeatable")
    parser.add_argument("--begin-day", default="", help="Start day YYYY-MM-DD")
    parser.add_argument("--end-day", default="", help="End day YYYY-MM-DD")
    parser.add_argument("--volume-tol", default="0", help="Allowed absolute minute-vs-daily volume diff")
    parser.add_argument("--volume-rel-tol", default="0", help="Allowed relative minute-vs-daily volume diff")
    parser.add_argument("--max-price-jump", default="0.25", help="Daily close jump threshold (default 0.25)")
    parser.add_argument("--max-minute-jump", default="0.15", help="Intraday minute jump threshold (default 0.15)")
    parser.add_argument("--oi-jump-ratio", default="5", help="Daily open-interest jump ratio threshold")
    parser.add_argument("--min-oi", default="100", help="Minimum previous OI for jump checks")
    parser.add_argument("--output", type=Path, default=None, help="Write full JSON report to this path")
    args = parser.parse_args(argv)

    if args.warehouse_dir is not None:
        store = LocalWarehouse(args.warehouse_dir, provider=args.provider)
    else:
        endpoint = Endpoint(args.terminal_data) if args.terminal_data else None
        lab_root = (
            args.lab_dir.parents[1]
            if args.lab_dir.name == Dataset.provider and args.lab_dir.parent.name == "warehouse-cache"
            else args.lab_dir
        )
        store = Dataset(lab_root, history=endpoint)

    report = audit(
        store,
        version_id=args.version,
        exchanges=set(args.exchange) if args.exchange else None,
        begin_day=args.begin_day,
        end_day=args.end_day,
        volume_tol=Decimal(args.volume_tol),
        volume_rel_tol=Decimal(args.volume_rel_tol),
        max_price_jump=Decimal(args.max_price_jump),
        max_minute_jump=Decimal(args.max_minute_jump),
        oi_jump_ratio=Decimal(args.oi_jump_ratio),
        min_oi=Decimal(args.min_oi),
    )
    print(format_report(report))
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    sys.exit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
