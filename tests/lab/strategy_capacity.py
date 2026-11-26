"""Measure synthetic history replay and rolling under Trading's Linux address-space limit."""
import argparse
import datetime
import json
import os
from pathlib import Path
import resource
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symbols", type=int, default=64)
    parser.add_argument("--bars", type=int, default=65536)
    parser.add_argument("--history-limit", type=int, default=0)
    parser.add_argument("--memory-limit-mb", type=int, default=4096)
    args = parser.parse_args()
    limit_bytes = args.memory_limit_mb * 1024 ** 2
    resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))
    for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS",
                 "VECLIB_MAXIMUM_THREADS", "NUMEXPR_NUM_THREADS"):
        os.environ[name] = "1"
    root = Path(__file__).resolve().parents[2]
    sys.path[:0] = [str(root / "apps/services/common/python"), str(root / "research")]
    import numpy
    import scipy
    from asterion_strategy import Bar, Context, observe
    from factors.price import momentum

    max_history = args.history_limit if args.history_limit > 0 else None
    context = Context([f"sample/{i}" for i in range(args.symbols)], max_history=max_history)
    started = time.perf_counter()
    report = dict(symbols=args.symbols, requested_bars=args.bars, completed_bars=0,
                  history_limit=max_history,
                  python=sys.version.split()[0], numpy=numpy.__version__, scipy=scipy.__version__,
                  address_space_mib=args.memory_limit_mb, status="complete", phase="replay")
    stamp = datetime.datetime(2026, 1, 1)

    def bars(contract):
        return [Bar(symbol, contract, stamp, 100., 100., 100., 100., 10., 100., 0.1)
                for symbol in context.symbols]

    try:
        for _ in range(args.bars):
            stamp += datetime.timedelta(minutes=1)
            observe(context, bars("first"))
            report["completed_bars"] += 1
        report["replay_seconds"] = time.perf_counter() - started
        report["phase"] = "factor"
        started = time.perf_counter()
        for symbol in context.symbols:
            context.factor(momentum, symbol, 21, lookback=20)
        report["factor_seconds"] = time.perf_counter() - started
        report["phase"] = "roll"
        started = time.perf_counter()
        context.rolls["second"] = 1.01
        stamp += datetime.timedelta(minutes=1)
        observe(context, bars("second"))
        report["roll_seconds"] = time.perf_counter() - started
    except MemoryError:
        report["status"] = "memory_exhausted"
        report[f"{report['phase']}_seconds"] = time.perf_counter() - started
    report["history_mib"] = sum(column.nbytes for fields in context.columns.values()
                                for column in fields.values()) / 1024 ** 2
    report["peak_rss_mib"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
    print(json.dumps(report, ensure_ascii=False))
    return 0 if report["status"] == "complete" else 1


if __name__ == "__main__":
    sys.exit(main())
