"""Isolated process verification: 32-product cross-session daily and 1-minute weight portfolios."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [
    str(source / "tests/trading"),
    str(source / "tests/support"),
    str(source / "apps/services/common/python"),
    str(build / "generated-python"),
]

from asterion import (  # noqa: E402
    common_pb2 as common,
    strategy_pb2 as wire,
    trading_pb2 as trading,
)
from strategy_process import (  # noqa: E402
    account_command,
    account_ready,
    account_rpc,
    account_snapshot,
    rpc,
    terminate,
    until,
)
from trading_installation import Programs, install  # noqa: E402

# 32 products spanning 21:00 night, 09:00 morning-only, and 09:30 CFFEX sessions.
NIGHT_CONTRACTS = [
    ("SHFE", "rb2701", "rb", "2027-01", -0.05, -1),  # seeded +2 -> flips to -1 at 21:00
    ("SHFE", "hc2701", "hc", "2027-01", 0.072, 1),   # seeded +1 -> drift 3.7% < 4% min_rebalance_weight keeps +1
    ("SHFE", "cu2612", "cu", "2026-12", 0.05, 1),
    ("SHFE", "al2612", "al", "2026-12", -0.05, -1),
    ("SHFE", "zn2612", "zn", "2026-12", 0.05, 1),
    ("SHFE", "au2612", "au", "2026-12", -0.05, -1),
    ("SHFE", "ag2612", "ag", "2026-12", 0.05, 1),
    ("SHFE", "ru2701", "ru", "2027-01", -0.05, -1),
    ("DCE", "i2701", "i", "2027-01", 0.05, 1),       # seeded +2 -> reduces to +1 at 21:00
    ("DCE", "m2701", "m", "2027-01", 0.05, 1),
    ("DCE", "y2701", "y", "2027-01", -0.05, -1),
    ("DCE", "p2701", "p", "2027-01", 0.05, 1),
    ("CZCE", "TA701", "TA", "2027-01", -0.05, -1),
    ("CZCE", "MA701", "MA", "2027-01", 0.05, 1),
    ("CZCE", "SA701", "SA", "2027-01", -0.05, -1),
    ("CZCE", "FG701", "FG", "2027-01", 0.0, 0),
]

MORNING_CONTRACTS = [
    ("DCE", "lh2701", "lh", "2027-01", 0.05, 1),     # seeded +2 -> reduces to +1 at 09:00
    ("DCE", "jd2701", "jd", "2027-01", 0.05, 1),
    ("DCE", "cs2701", "cs", "2027-01", -0.05, -1),
    ("DCE", "l2701", "l", "2027-01", 0.05, 1),
    ("CZCE", "AP701", "AP", "2027-01", 0.0, 0),      # seeded +2 -> closes to 0 at 09:00
    ("CZCE", "CJ701", "CJ", "2027-01", -0.05, -1),
    ("CZCE", "PK701", "PK", "2027-01", 0.05, 1),
    ("CZCE", "UR701", "UR", "2027-01", -0.05, -1),
    ("GFEX", "si2701", "si", "2027-01", 0.05, 1),
    ("GFEX", "lc2701", "lc", "2027-01", 0.0, 0),     # zero weight & zero position: never receives live ticks
]

CFFEX_CONTRACTS = [
    ("CFFEX", "IF2612", "IF", "2026-12", -0.05, -1), # seeded +2 -> flips to -1 at 09:30
    ("CFFEX", "IC2612", "IC", "2026-12", -0.05, -1),
    ("CFFEX", "IM2612", "IM", "2026-12", -0.05, -1),
    ("CFFEX", "IH2612", "IH", "2026-12", 0.05, 1),
    ("CFFEX", "T2612", "T", "2026-12", -0.05, -1),
    ("CFFEX", "TF2612", "TF", "2026-12", -0.022, 0),
]

ALL_CONTRACTS = NIGHT_CONTRACTS + MORNING_CONTRACTS + CFFEX_CONTRACTS
assert len(ALL_CONTRACTS) == 32

SEEDED_LONGS = {
    ("SHFE", "rb2701"): 2,
    ("SHFE", "hc2701"): 1,
    ("DCE", "i2701"): 3,
    ("DCE", "lh2701"): 3,
    ("CZCE", "AP701"): 2,
    ("CFFEX", "IF2612"): 2,
}


def symbol_key(exch: str, prod: str, ym: str) -> str:
    return f"{exch}/{prod.lower()}/{ym}"


WEIGHT_MAP = {
    symbol_key(exch, prod, ym): weight
    for exch, _, prod, ym, weight, _ in ALL_CONTRACTS
}

STRATEGY_CODE = f"""from asterion_strategy import Strategy

WEIGHTS = {json.dumps(WEIGHT_MAP)}

class CrossSessionPortfolio(Strategy):
    params = {{"once": False}}

    def ready(self, ctx):
        return True

    def on_bars(self, ctx, bars):
        sym = next(iter(bars))
        n = len(ctx.history(sym, "close", 10))
        if n <= 1:
            return
        if self.params["once"] and n > 2:
            return
        ctx.weights(WEIGHTS)
"""


def setup_host(directory: Path):
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    host = install(
        directory,
        Programs(
            str(build / "asterion_trading"),
            str(build / "asterion_test_certificates"),
            str(build / f"libasterion_test_ctp_trader{suffix}"),
        ),
    )
    (host.root / "lib/ctp-md.so").symlink_to(build / f"libasterion_test_ctp{suffix}")
    code = host.root / "lib/strategy"
    (code / "asterion").mkdir(parents=True)
    for path in (
        source / "apps/services/trading/python/strategy_runner.py",
        source / "apps/services/common/python/asterion_strategy.py",
    ):
        shutil.copy2(path, code)
    for name in ("strategy_pb2.py", "common_pb2.py"):
        shutil.copy2(build / "generated-python/asterion" / name, code / "asterion")
    (host.root / "runtime").mkdir()
    (host.root / "runtime/python").symlink_to(Path(sys.executable).resolve().parents[1])
    host.account("account", user="script")
    return host


def open_and_seed(host):
    contracts = [
        trading.ContractTerms(
            instrument=common.Instrument(exchange=exch, symbol=sym),
            product=prod,
            delivery_month=ym,
            multiplier=common.Decimal(text="10"),
            price_tick=common.Decimal(text="1"),
            lot_size=common.Decimal(text="1"),
        )
        for exch, sym, prod, ym, _, _ in ALL_CONTRACTS
    ]
    account_rpc(
        host,
        trading.Request(
            open_account=trading.Policy(
                limits=common.RiskLimits(
                    max_order_lots=common.Decimal(text="5"),
                    max_contract_lots=common.Decimal(text="10"),
                    max_gross_lots=common.Decimal(text="60"),
                    max_working_orders=16,
                ),
                max_price_deviation=common.Decimal(text="0.05"),
                contracts=contracts,
            )
        ),
    )
    account_ready(host)
    account_command(
        host,
        trading.Command(
            request_id="auth.seed",
            authorize=trading.Authorize(user_id="script"),
        ),
    )
    seeded = 0
    for idx, ((exch, sym), total_lots) in enumerate(SEEDED_LONGS.items()):
        rem = total_lots
        part = 0
        while rem > 0:
            chunk = min(rem, 2)
            rem -= chunk
            part += 1
            oid = f"seed.{idx}.{part}.{sym}"
            account_command(
                host,
                trading.Command(
                    request_id=oid,
                    submit_order=trading.SubmitOrder(
                        order_id=oid,
                        instrument=common.Instrument(exchange=exch, symbol=sym),
                        side=common.SIDE_BUY,
                        offset=common.OFFSET_OPEN,
                        lots=common.Decimal(text=str(chunk)),
                        price=common.Decimal(text="3501"),
                    ),
                ),
            )
            seeded += chunk
            until(
                lambda: account_snapshot(host),
                lambda s, expected=seeded: s.positions_reconciled
                and sum(
                    int(p.today_lots.text) + int(p.yesterday_lots.text)
                    for p in s.positions
                )
                == expected,
            )
    account_ready(host)


def publish_and_start(host, instance_id: str, period: int, once: bool):
    manifest = wire.Manifest(
        name="cross-session",
        version="1",
        entry="portfolio:CrossSessionPortfolio",
        runtime="python3.12-numpy2.3.5-scipy1.17.1",
        files=["portfolio.py"],
    )
    rpc(
        host,
        "publish_strategy",
        wire.Package(
            manifest=manifest,
            files=[wire.File(path="portfolio.py", content=STRATEGY_CODE.encode())],
        ),
    )
    warmup = wire.Warmup(dataset_version="v-20260925")
    digest = hashlib.sha256()
    batch = warmup.bars.add(sequence=1)
    digest.update(b"batch\n")
    for exch, sym, _, _, _, _ in ALL_CONTRACTS:
        batch.bars.add(
            instrument=common.Instrument(exchange=exch, symbol=sym),
            trading_day="2026-09-25",
            end_ms=1758787200000,
            open=common.Decimal(text="3500"),
            high=common.Decimal(text="3500"),
            low=common.Decimal(text="3500"),
            close=common.Decimal(text="3500"),
            volume=100,
            open_interest=common.Decimal(text="1000"),
        )
        digest.update(f"2026-09-25|1758787200000|{exch}/{sym}|3500|3500|3500|3500|100|1000\n".encode())
    warmup.sha256 = digest.hexdigest()

    cfg = wire.Instance(
        id=instance_id,
        package_name="cross-session",
        package_version="1",
        instruments=[common.Instrument(exchange=exch, symbol=sym) for exch, sym, _, _, _, _ in ALL_CONTRACTS],
        period=period,
        params={"once": wire.ParameterValue(flag=once)},
        weight_limits=wire.WeightLimits(
            gross=common.Decimal(text="2"),
            net=common.Decimal(text="0"),
            min_rebalance_weight=common.Decimal(text="0.04"),
        ),
        warmup=warmup,
    )
    started = rpc(host, "start_strategy", cfg).instance
    assert started.state == trading.INSTANCE_STATE_READY, started


def append_ticks(flow: Path, contracts, tday: str, vol: int):
    flow.mkdir(parents=True, exist_ok=True)
    with (flow / "script.tsv").open("a") as out:
        for exch, sym, *_ in contracts:
            out.write(
                f"{exch}\t{sym}\t{tday}\t3500\t3499\t3501\t{vol}\t5000\t4900\t3490\t3800\t3200\n"
            )


def stream_opening_ticks(flow: Path, contracts, tday: str, vol: int):
    # Bar delivery can wait ten seconds for contracts whose sessions are still closed.
    end = time.monotonic() + 10.5
    while time.monotonic() < end:
        append_ticks(flow, contracts, tday, vol)
        time.sleep(0.2)


def roll_trader_day(trader_flow: Path, next_day: str, host):
    target = trader_flow / "next-day"
    tmp = trader_flow / "next-day.tmp"
    tmp.write_text(next_day)
    tmp.replace(target)
    until(
        lambda: account_snapshot(host),
        lambda s: (
            not target.exists()
            and s.trading_day == f"{next_day[:4]}-{next_day[4:6]}-{next_day[6:]}"
            and s.broker_state == trading.BROKER_STATE_READY
            and s.positions_reconciled
        ),
    )
    account_ready(host)


def net_positions(host) -> dict[tuple[str, str], int]:
    snap = account_snapshot(host)
    if not snap.positions_reconciled:
        return {}
    result: dict[tuple[str, str], int] = {}
    for pos in snap.positions:
        lots = int(pos.today_lots.text) + int(pos.yesterday_lots.text)
        if lots == 0:
            continue
        key = (pos.instrument.exchange, pos.instrument.symbol)
        signed = lots if pos.side == common.SIDE_BUY else -lots
        result[key] = result.get(key, 0) + signed
    return {k: v for k, v in result.items() if v != 0}


def run_daily_portfolio(base_dir: Path):
    host = setup_host(base_dir / "daily")
    md_flow = host.root / "data/account/strategies/ctp-flow"
    trader_flow = host.root / "data/account/ctp-flow"
    md_flow.mkdir(parents=True, exist_ok=True)
    (md_flow / "script.tsv").write_text("")
    proc = host.start("account")
    try:
        open_and_seed(host)
        publish_and_start(host, "daily-32", common.PERIOD_DAY, once=False)
        time.sleep(0.4)

        # Seed partial day 20260928, then complete day 20260929 across active contracts (except lc2701).
        active_31 = [c for c in ALL_CONTRACTS if c[1] != "lc2701"]
        append_ticks(md_flow, active_31, "20260928", 100)
        time.sleep(0.5)
        roll_trader_day(trader_flow, "20260929", host)
        append_ticks(md_flow, active_31, "20260929", 200)
        time.sleep(0.5)

        # Roll counter to 20260930; at 21:00 ONLY night contracts open!
        roll_trader_day(trader_flow, "20260930", host)
        stream_opening_ticks(md_flow, NIGHT_CONTRACTS, "20260930", 100)
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260930", 150)

        expected_after_night = {
            **{(exch, sym): exp for exch, sym, _, _, _, exp in NIGHT_CONTRACTS if exp != 0},
            ("DCE", "lh2701"): 3,
            ("CZCE", "AP701"): 2,
            ("CFFEX", "IF2612"): 2,
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_after_night,
            seconds=20,
        )

        # Wait > 10.5s past the 21:00 decision so any global 10s batch timer would have expired,
        # then stream 09:00 morning-only contracts (excluding zero-weight zero-position lc2701!).
        time.sleep(10.5)
        morning_quoted = [c for c in MORNING_CONTRACTS if c[1] != "lc2701"]
        append_ticks(md_flow, morning_quoted, "20260930", 100)
        expected_after_morning = {
            **{(exch, sym): exp for exch, sym, _, _, _, exp in NIGHT_CONTRACTS + MORNING_CONTRACTS if exp != 0},
            ("CFFEX", "IF2612"): 2,
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_after_morning,
            seconds=20,
        )

        # 09:30 CFFEX session opens and completes remaining reductions and openings.
        append_ticks(md_flow, CFFEX_CONTRACTS, "20260930", 100)
        expected_final = {
            (exch, sym): exp for exch, sym, _, _, _, exp in ALL_CONTRACTS if exp != 0
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_final,
            seconds=20,
        )

        inst = rpc(host, "get_strategies", {}).instance
        assert inst.state == trading.INSTANCE_STATE_READY, inst
        abandoned = [
            e for e in inst.events
            if e.kind == trading.INSTANCE_EVENT_KIND_BATCH_ABANDONED
        ]
        assert not abandoned, abandoned
    finally:
        terminate(proc)


def minute_bucket_id() -> int:
    now = int(time.time())
    bj_min = ((now + 8 * 3600) % 86400) // 60
    base = now // 60
    if 21 * 60 - 5 <= bj_min < 21 * 60:
        return base + (21 * 60 - bj_min)
    if 9 * 60 - 5 <= bj_min < 9 * 60:
        return base + (9 * 60 - bj_min)
    return base


def run_minute_portfolio(base_dir: Path):
    host = setup_host(base_dir / "minute")
    md_flow = host.root / "data/account/strategies/ctp-flow"
    md_flow.mkdir(parents=True, exist_ok=True)
    (md_flow / "script.tsv").write_text("")
    proc = host.start("account")
    try:
        open_and_seed(host)
        publish_and_start(host, "minute-32", common.PERIOD_MINUTES_1, once=True)
        time.sleep(0.4)

        # Minute M0: initialize partial first minute bucket on night contracts ONLY.
        m0 = minute_bucket_id()
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 100)
        while minute_bucket_id() <= m0:
            time.sleep(0.2)

        # Minute M1: complete minute bucket on night contracts ONLY (morning & CFFEX stay closed).
        m1 = minute_bucket_id()
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 200)
        while minute_bucket_id() <= m1:
            time.sleep(0.2)

        # Minute M2 + 10.5s freshness grace: deliver M1 bar and execute night contracts!
        stream_opening_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 300)
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 350)

        expected_after_night = {
            **{(exch, sym): exp for exch, sym, _, _, _, exp in NIGHT_CONTRACTS if exp != 0},
            ("DCE", "lh2701"): 3,
            ("CZCE", "AP701"): 2,
            ("CFFEX", "IF2612"): 2,
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_after_night,
            seconds=20,
        )

        # Advance one more minute so an empty ready decision (`once=True`) arrives on M2,
        # proving pending morning & CFFEX weights survive intermediate minute bars!
        m2 = minute_bucket_id()
        while minute_bucket_id() <= m2:
            time.sleep(0.2)
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 400)
        time.sleep(10.5)
        append_ticks(md_flow, NIGHT_CONTRACTS, "20260928", 450)
        until(
            lambda: rpc(host, "get_strategies", {}).instance,
            lambda inst: inst.completed_steps >= 3,
            seconds=20,
        )

        # 09:00 morning-only contracts open (lc2701 stays unquoted).
        morning_quoted = [c for c in MORNING_CONTRACTS if c[1] != "lc2701"]
        append_ticks(md_flow, morning_quoted, "20260928", 100)
        expected_after_morning = {
            **{(exch, sym): exp for exch, sym, _, _, _, exp in NIGHT_CONTRACTS + MORNING_CONTRACTS if exp != 0},
            ("CFFEX", "IF2612"): 2,
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_after_morning,
            seconds=20,
        )

        # 09:30 CFFEX contracts open.
        append_ticks(md_flow, CFFEX_CONTRACTS, "20260928", 100)
        expected_final = {
            (exch, sym): exp for exch, sym, _, _, _, exp in ALL_CONTRACTS if exp != 0
        }
        until(
            lambda: net_positions(host),
            lambda pos: pos == expected_final,
            seconds=20,
        )

        inst = rpc(host, "get_strategies", {}).instance
        assert inst.state == trading.INSTANCE_STATE_READY, inst
        abandoned = [
            e for e in inst.events
            if e.kind == trading.INSTANCE_EVENT_KIND_BATCH_ABANDONED
        ]
        assert not abandoned, abandoned
    finally:
        terminate(proc)


def main():
    with tempfile.TemporaryDirectory(prefix="ast-cross-session-") as tmp:
        base = Path(tmp)
        errors: list[BaseException] = []

        def run_guarded(fn):
            try:
                fn(base)
            except BaseException as exc:
                errors.append(exc)

        t_day = threading.Thread(target=run_guarded, args=(run_daily_portfolio,))
        t_min = threading.Thread(target=run_guarded, args=(run_minute_portfolio,))
        t_day.start()
        t_min.start()
        t_day.join()
        t_min.join()
        if errors:
            raise errors[0]


if __name__ == "__main__":
    main()
