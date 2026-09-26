"""Isolated synthetic monthly-merge benchmark; never opens the desktop environment.

Seed and measure in separate processes so peak RSS excludes fixture construction.
Example: uv run python scripts/benchmark_history_merge.py seed --root /tmp/history-bench --months 120 --contracts 500
         uv run python scripts/benchmark_history_merge.py measure --root /tmp/history-bench --output /tmp/history-result.json
The measurement rolls back catalogue changes. Its output and fixture root cannot be overwritten.
"""

import argparse
import json
import os
import platform
import resource
import time
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
from asterion_bindings.data_partitions import cumulative_series
from asterion_bindings.database import create_engine

from asterion.data import partitions
from asterion.data.library import DataLibrary, versions
from asterion.distribution_storage import data_storage
from asterion.platform.serialization import canonical


def rows_for(month, contracts):
    return [
        {
            "symbol": f"TEST{i}",
            "contract": f"SHFE.TEST{i}",
            "exchange": "SHFE",
            "trading_day": f"{month}-{day:02}",
            "open": "100",
            "high": "101",
            "low": "99",
            "close": "100",
            "vol": "100",
            "oi": "200",
        }
        for day in range(1, 21)
        for i in range(contracts)
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["seed", "measure"])
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--months", type=int, default=120)
    parser.add_argument("--contracts", type=int, default=500)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.mode == "seed":
        if not 1 <= args.months <= 1200 or not 1 <= args.contracts <= 10000:
            parser.error("Fixture dimensions are out of bounds")
        root.mkdir(parents=True, exist_ok=False)
    elif not (root / "fixture.json").is_file() or args.output is None:
        parser.error("An explicit synthetic fixture and report path are required")
    engine = create_engine(f"sqlite:///{root}/catalog.db")
    library = DataLibrary(data_storage(engine), root)
    scope = {"benchmark": "synthetic-multiple-contracts"}
    if args.mode == "seed":
        parts = []
        for offset in range(args.months):
            month = f"{2000 + offset // 12:04}-{offset % 12 + 1:02}"
            rows = rows_for(month, args.contracts)
            library.types.get("futures.daily").validate(rows)
            values = [
                r
                | {"_observed_at": "2026-01-01T00:00:00+00:00", "_raw_version_id": "synthetic-raw"}
                for r in rows
            ]
            output = pa.BufferOutputStream()
            pq.write_table(pa.Table.from_pylist(values), output)
            # pyarrow-written, as partitions published before the Rust merge were;
            # "artifacts/" is the current data-store partition layout.
            stored = library.artifacts.put_addressed(
                "artifacts", ".parquet", output.getvalue().to_pybytes()
            )
            parts.append(
                {
                    "key": month,
                    "checksum": stored.sha256,
                    "rows": len(rows),
                    "bytes": stored.bytes,
                    "first": f"{month}-01",
                    "last": f"{month}-20",
                    "inputs": ["synthetic-raw"],
                    "replaces": None,
                }
            )
        index = library.artifacts.put(
            "parent.json", canonical({"schema_version": 1, "partitions": parts})
        )
        with engine.begin() as conn:
            dataset = library.ensure_collection(
                conn,
                "futures.daily",
                "benchmark",
                scope,
                "STANDARD",
                cumulative_series("futures.daily"),
            )
            conn.execute(
                versions.insert().values(
                    id="synthetic-parent",
                    dataset_id=dataset,
                    job_id="seed",
                    created_at=1,
                    rows=sum(p["rows"] for p in parts),
                    manifest={
                        "path": index.name,
                        "checksum": index.sha256,
                        "bytes": index.bytes,
                        "partitions": parts,
                        "revision": 1,
                        "observed_at": "2026-01-01T00:00:00+00:00",
                        "first": parts[0]["first"],
                        "last": parts[-1]["last"],
                        "coverage_gaps": None,
                    },
                )
            )
        (root / "fixture.json").write_text(
            json.dumps(
                {
                    "months": args.months,
                    "contracts": args.contracts,
                    "rows": args.months * args.contracts * 20,
                    "last_month": parts[-1]["key"],
                }
            )
        )
        print(root)
    else:
        fixture = json.loads((root / "fixture.json").read_text())
        row = rows_for(fixture["last_month"], 1)[0] | {"close": "101"}
        definition = library.types.get("futures.daily").manifest
        before = {p.name: p.stat().st_size for p in (root / "artifacts").iterdir()}
        started = time.perf_counter()
        with engine.connect() as conn:
            transaction = conn.begin()
            _, _, result = partitions.prepare(
                library,
                conn,
                job_id="measure",
                type_id="futures.daily",
                source="benchmark",
                scope=scope,
                rows=[row],
                observed_by_key={
                    tuple(row[k] for k in definition.primary_key): "2026-01-02T00:00:00+00:00"
                },
            )
            transaction.rollback()
        elapsed = time.perf_counter() - started
        # Reported by the Rust merge itself: partitions actually decoded.
        counters = {
            "partition_reads": len(result["metrics"]["partitions_read"]),
            "partition_bytes_read": result["metrics"]["bytes_read"],
        }
        after = {p.name: p.stat().st_size for p in (root / "artifacts").iterdir()}
        report = {
            "scope": "synthetic_merge_only_not_live_publication",
            "cache": "OS cache uncontrolled; no cache eviction",
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu": platform.processor(),
            "logical_cpus": os.cpu_count(),
            "physical_memory_bytes": os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE"),
            "fixture": fixture,
            **counters,
            "wall_seconds": elapsed,
            "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            "artifact_count": len(before),
            "new_artifact_bytes": sum(size for name, size in after.items() if name not in before),
            "manifest_bytes": (root / "parent.json").stat().st_size,
            "result_rows": result["rows"],
            "changes": result["detail"]["changes"],
        }
        with args.output.open("x") as output:
            json.dump(report, output, indent=2)
        print(json.dumps(report))
    engine.dispose()


if __name__ == "__main__":
    main()
