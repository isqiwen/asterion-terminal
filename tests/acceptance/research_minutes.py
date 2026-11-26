"""Audit a short range of real minutes from one fixed Warehouse version."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import sys


def windows(ends, minutes, timezone):
    """Consecutive period-end labels, in Beijing time."""
    ranges = []
    for end in ends:
        if ranges and end == ranges[-1][1] + minutes * 60_000:
            ranges[-1][1] = end
        else:
            ranges.append([end, end])
    return [[datetime.datetime.fromtimestamp(ms / 1000, timezone).isoformat(timespec="minutes")
             for ms in pair] for pair in ranges]


def main(argv=None):
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=root / "build/Debug")
    parser.add_argument("--lab-directory", type=Path, required=True)
    parser.add_argument("--terminal-data", type=Path, help="Terminal data directory for uncached versions")
    parser.add_argument("--version", required=True)
    parser.add_argument("--contract", required=True)
    parser.add_argument("--first-day", type=datetime.date.fromisoformat, required=True)
    parser.add_argument("--last-day", type=datetime.date.fromisoformat, required=True)
    parser.add_argument("--minutes", type=int, default=5)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv)
    if "asterion_lab" not in sys.modules:
        sys.path[:0] = [str(root / "apps/services/lab"), str(root / "apps/services/common/python"),
                        str(args.build / "generated-python"), str(args.build / "engine-python")]
    from asterion_lab import chart, data as lab_data
    from asterion_lab.version import COMMIT
    from asterion_lab.warehouse import write_durably
    from asterion_lab.warehouse_client import Dataset, Endpoint

    parts = args.contract.split("/")
    if args.last_day < args.first_day or args.minutes not in chart.MINUTES or len(parts) != 3 or not all(parts):
        parser.error("select a valid EXCHANGE/product/YYYY-MM contract, ordered date range and supported minute period")
    exchange, product, month = parts
    endpoint = Endpoint(args.terminal_data) if args.terminal_data else None
    report = dict(analysis_commit=COMMIT, dataset_version=args.version, contract=args.contract, period_minutes=args.minutes,
                  first_day=args.first_day.isoformat(), last_day=args.last_day.isoformat(), days=[])
    with Dataset(args.lab_directory, history=endpoint) as dataset:
        data = lab_data.Data(dataset, report["first_day"], report["last_day"], version=args.version)
        calendar = data.calendar(exchange)
        life = data.contract_life(exchange, args.contract)
        report["listed_life"] = [date.isoformat() for date in life] if life else None
        daily = data.product(exchange, product).get(args.contract)
        complete, opened, volumes_match = life is not None, False, True
        for offset in range((args.last_day - args.first_day).days + 1):
            date = args.first_day + datetime.timedelta(days=offset)
            day = date.isoformat()
            row = daily.days.get(day) if daily else None
            key = f"minutes/{exchange}/{day}/{product}/{month}"
            part = data.version.manifest["partitions"].get(key)
            volume = row["volume"] if row else None
            settled = row["settlement"] is not None if row else False
            entry = dict(day=day, calendar_open=calendar.get(day), daily_row=row is not None,
                         daily_settled=settled,
                         daily_volume=str(volume) if volume is not None else None, minute_partition=None)
            report["days"].append(entry)
            in_life = not life or life[0] <= date <= life[1]
            if in_life and calendar.get(day):
                opened = True
            else:
                if (in_life and calendar.get(day) is None) or row or part:
                    complete = False
                if row is None and not part:
                    continue
            rows = list(data.minute_rows(args.contract, day, day))
            if part:
                with (dataset.root / part["file"]).open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
                if digest != part["sha256"] or len(rows) != part["rows"]:
                    raise ValueError(f"{key} differs from its pinned manifest")
                entry["minute_partition"] = dict(key=key, sha256=digest, rows=len(rows))
            if volume == 0 and settled and not rows:
                entry["zero_volume_day"] = True
                continue
            listed = life[0] if life else None
            bars, coverage = chart.minute_coverage(day, rows, args.minutes, args.contract, calendar, listed)
            raw = coverage
            if args.minutes != 1:
                _, raw = chart.minute_coverage(day, rows, 1, args.contract, calendar, listed)
            volumes = [r[7] for r in rows]
            observed_volume = sum(volumes) if volumes and None not in volumes else None
            difference = observed_volume - volume if observed_volume is not None and volume is not None else None
            entry.update(complete_priced_bars=sum(not bar["partial"] for bar in bars),
                         recorded_volume=str(observed_volume) if observed_volume is not None else None,
                         volume_difference=str(difference) if difference is not None else None,
                         missing_minute_ends=windows(raw["missing"], 1, chart.BEIJING),
                         source_partial_minutes=sum(bool(r[2]) for r in rows),
                         zero_volume_minutes=coverage["zero_volume_minutes"], unchecked_session=coverage["unchecked"],
                         partial_periods=len(coverage["partial"]), missing_periods=len(coverage["missing"]),
                         partial_ends=windows(coverage["partial"], args.minutes, chart.BEIJING),
                         missing_ends=windows(coverage["missing"], args.minutes, chart.BEIJING))
            if difference != 0:
                volumes_match = False
            if not rows or not settled or coverage["unchecked"] or coverage["partial"] or coverage["missing"]:
                complete = False
    report["minute_coverage_complete"] = complete and opened
    report["passed"] = report["minute_coverage_complete"] and volumes_match
    write_durably(args.report, (json.dumps(report, indent=2, ensure_ascii=False) + "\n").encode())
    print(f"{args.report}: {'passed' if report['passed'] else 'incomplete'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
