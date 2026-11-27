"""Point-in-time (PIT) alignment for exchange fundamental and member-position tables.

Exchange post-close reports (warehouse_receipts, member_positions, inventory)
dated `report_day` are compiled after the 15:00 Beijing close. A report becomes
visible only on bars completing strictly after 15:00 on `report_day` AND at or
after `published_ms` when recorded, preventing same-day close lookahead.
"""
from bisect import bisect_right
import datetime
import functools
import math

import polars as pl

_EPOCH_ORDINAL = 719163
_SUMMARY_PATTERN = "合计|总计|小计"
_DAILY_MAX_AGE_DAYS = 14
_WEEKLY_MAX_AGE_DAYS = 21


@functools.lru_cache(maxsize=8192)
def _day_ordinal(day: str) -> int:
    return datetime.date.fromisoformat(day).toordinal()


def _close_ms(day: datetime.date) -> int:
    return (day.toordinal() - _EPOCH_ORDINAL) * 86_400_000 + 7 * 3_600_000


def _available_ms(day: datetime.date, published_ms) -> int:
    post_close = _close_ms(day) + 1
    if published_ms is None:
        return post_close
    return max(int(published_ms), post_close)


def _is_summary_expr(*columns: str) -> pl.Expr:
    expr = pl.lit(False)
    for col in columns:
        expr = expr | pl.col(col).str.contains(_SUMMARY_PATTERN).fill_null(False)
    return expr


def _load_detail_or_summary_timeline(
    warehouse, version, table: str, exchange: str, lookback: str, end: str,
    value_col: str, label_cols: tuple[str, ...],
) -> dict:
    if not any(version.keys(table, exchange=exchange, begin=lookback, end=end)):
        return {}
    begin_d, end_d = datetime.date.fromisoformat(lookback), datetime.date.fromisoformat(end)
    val = pl.col(value_col).cast(pl.Float64)
    valid = val.is_not_null() & val.is_finite()
    is_sum = _is_summary_expr(*label_cols)
    df = (
        warehouse.scan(version, table, exchange=exchange, begin=lookback, end=end)
        .filter((pl.col("exchange") == exchange) & pl.col("day").is_between(begin_d, end_d))
        .select(
            "product",
            "day",
            pl.col("published_ms"),
            pl.when(valid & ~is_sum).then(val).alias("_detail"),
            pl.when(valid & is_sum).then(val).alias("_summary"),
        )
        .group_by(["product", "day"])
        .agg(
            pl.col("_detail").count().alias("n_detail"),
            pl.col("_detail").sum().alias("detail_sum"),
            pl.col("_summary").count().alias("n_summary"),
            pl.col("_summary").sum().alias("summary_sum"),
            pl.col("published_ms").max().alias("pub_ms"),
        )
        .filter((pl.col("n_detail") > 0) | (pl.col("n_summary") > 0))
        .collect()
    )
    records: dict[str, list] = {}
    for product, day, n_detail, detail_sum, _n_summary, summary_sum, pub_ms in df.iter_rows():
        total = float(detail_sum) if n_detail > 0 else float(summary_sum)
        records.setdefault(product, []).append((_available_ms(day, pub_ms), day.toordinal(), total))
    return _build_timeline(records)


def _build_timeline(records: dict) -> dict:
    """Turn `{product: [(available_ms, day_ord, payload), ...]}` into bisect-ready timelines."""
    out = {}
    for product, items in records.items():
        items.sort(key=lambda item: (item[0], item[1]))
        # Keep the latest report_day if multiple reports share an identical available_ms.
        deduped = []
        for avail, day_ord, payload in items:
            if deduped and deduped[-1][0] == avail:
                deduped[-1] = (avail, day_ord, payload)
            else:
                deduped.append((avail, day_ord, payload))
        out[product] = (
            [item[0] for item in deduped],
            [item[1] for item in deduped],
            [item[2] for item in deduped],
        )
    return out


class ExchangePit:
    """PIT fundamental timelines for one exchange over a date window."""

    def __init__(self, receipts: dict, members: dict, inventory: dict):
        self._receipts = receipts
        self._members = members
        self._inventory = inventory

    def empty(self) -> bool:
        return not (self._receipts or self._members or self._inventory)

    def lookup(self, product: str, bar_day: str, bar_end_ms: int) -> tuple[float, float, float, float]:
        """Return `(receipt_volume, member_long, member_short, inventory_total)` visible at `(bar_day, bar_end_ms)`."""
        if self.empty():
            return math.nan, math.nan, math.nan, math.nan
        day_ord = _day_ordinal(bar_day)
        receipt = self._latest(self._receipts, product, day_ord, bar_end_ms, _DAILY_MAX_AGE_DAYS)
        member = self._latest(self._members, product, day_ord, bar_end_ms, _DAILY_MAX_AGE_DAYS)
        inv = self._latest(self._inventory, product, day_ord, bar_end_ms, _WEEKLY_MAX_AGE_DAYS)
        m_long, m_short = member if member is not None else (math.nan, math.nan)
        return (
            receipt if receipt is not None else math.nan,
            m_long,
            m_short,
            inv if inv is not None else math.nan,
        )

    @staticmethod
    def _latest(table: dict, product: str, day_ord: int, bar_end_ms: int, max_age_days: int):
        timeline = table.get(product)
        if timeline is None:
            return None
        times, day_ords, payloads = timeline
        idx = bisect_right(times, bar_end_ms) - 1
        if idx < 0:
            return None
        if day_ord - day_ords[idx] > max_age_days:
            return None
        return payloads[idx]


EMPTY_PIT = ExchangePit({}, {}, {})


def load_exchange_pit(warehouse, version, exchange: str, begin: str, end: str) -> ExchangePit:
    """Load PIT fundamentals for `exchange` over `[begin - 30d, end]`.

    Skips querying any table absent from the pinned version so price-only
    datasets incur zero extra Parquet scans.
    """
    first = datetime.date.fromisoformat(begin)
    lookback = (first - min(datetime.timedelta(days=30), first - datetime.date.min)).isoformat()

    receipts = _load_detail_or_summary_timeline(
        warehouse, version, "warehouse_receipts", exchange, lookback, end, "volume", ("warehouse", "area")
    )

    members_raw: dict[str, list] = {}
    if any(version.keys("member_positions", exchange=exchange, begin=lookback, end=end)):
        begin_d, end_d = datetime.date.fromisoformat(lookback), datetime.date.fromisoformat(end)
        lp = pl.col("long_position").cast(pl.Float64)
        sp = pl.col("short_position").cast(pl.Float64)
        lp_ok = pl.when(lp.is_not_null() & lp.is_finite()).then(lp)
        sp_ok = pl.when(sp.is_not_null() & sp.is_finite()).then(sp)
        is_prod = pl.col("contract").is_null() | (pl.col("contract") == "")
        df = (
            warehouse.scan(version, "member_positions", exchange=exchange, begin=lookback, end=end)
            .filter(
                (pl.col("exchange") == exchange)
                & pl.col("day").is_between(begin_d, end_d)
                & pl.col("member").is_not_null()
                & (pl.col("member") != "")
                & ~_is_summary_expr("member")
            )
            .select(
                "product",
                "day",
                is_prod.alias("_is_prod"),
                pl.col("published_ms"),
                lp_ok.alias("_lp"),
                sp_ok.alias("_sp"),
            )
            .group_by(["product", "day"])
            .agg(
                pl.col("_is_prod").any().alias("has_prod"),
                pl.when(pl.col("_is_prod")).then(pl.col("_lp")).count().alias("p_nl"),
                pl.when(pl.col("_is_prod")).then(pl.col("_lp")).sum().alias("p_l"),
                pl.when(pl.col("_is_prod")).then(pl.col("_sp")).count().alias("p_ns"),
                pl.when(pl.col("_is_prod")).then(pl.col("_sp")).sum().alias("p_s"),
                pl.when(pl.col("_is_prod")).then(pl.col("published_ms")).max().alias("p_pub"),
                pl.col("_lp").count().alias("a_nl"),
                pl.col("_lp").sum().alias("a_l"),
                pl.col("_sp").count().alias("a_ns"),
                pl.col("_sp").sum().alias("a_s"),
                pl.col("published_ms").max().alias("a_pub"),
            )
            .collect()
        )
        for product, day, has_prod, p_nl, p_l, p_ns, p_s, p_pub, a_nl, a_l, a_ns, a_s, a_pub in df.iter_rows():
            nl, l_sum, ns, s_sum, pub = (
                (p_nl, p_l, p_ns, p_s, p_pub) if has_prod else (a_nl, a_l, a_ns, a_s, a_pub)
            )
            if nl == 0 and ns == 0:
                continue
            avail = _available_ms(day, pub)
            members_raw.setdefault(product, []).append((
                avail,
                day.toordinal(),
                (float(l_sum) if nl > 0 else math.nan, float(s_sum) if ns > 0 else math.nan),
            ))

    inventory = _load_detail_or_summary_timeline(
        warehouse, version, "inventory", exchange, lookback, end, "total", ("area", "warehouse")
    )

    return ExchangePit(receipts, _build_timeline(members_raw), inventory)
