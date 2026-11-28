"""Live Trading <-> Lab backtest reconciliation orchestration, RPC and CLI."""

import argparse
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import sys

from asterion.strategy_pb2 import Instance
from asterion_lab import backtest, reconcile_diff, reconcile_live, repository
from asterion_lab.runs import Runs
from asterion_lab.warehouse_client import Dataset
from asterion_lab.warmup import update_batch_digest


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


def _warmup_steps(instance: Instance, steps: list) -> int:
    if instance.HasField("warmup_steps"):
        return instance.warmup_steps
    if not instance.local_bars and not instance.resume_from:
        if not instance.HasField("warmup"):
            return 0
        digest = hashlib.sha256()
        for sequence, batch, _ in steps:
            update_batch_digest(digest, batch)
            if digest.hexdigest() == instance.warmup.sha256:
                return sequence
    raise ValueError("strategy run has no verifiable warmup boundary")


def _extract_live_window(
    instance: Instance,
    steps: list,
    recorded_raw: list,
    native_to_symbol: dict,
    native_to_contract: dict,
    begin_day: str,
    end_day: str,
) -> dict:
    warmup_steps = _warmup_steps(instance, steps)
    first_live = next(
        (batch.bars[0] for seq, batch, _ in steps if seq > warmup_steps and batch.bars), None
    )
    execute_from_ms = first_live.end_ms if first_live is not None else steps[-1][1].bars[0].end_ms + 1
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
            symbol = native_to_symbol.get(native) or reconcile_live.canonical_contract(
                bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
            )
            contract = native_to_contract.get(native) or reconcile_live.canonical_contract(
                bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
            )
            live_step_bars[(end_ms, symbol)] = {
                "end_ms": end_ms,
                "symbol": symbol,
                "contract": contract,
                "trading_day": bar.trading_day,
                "open": reconcile_live.wire_decimal(bar.open),
                "high": reconcile_live.wire_decimal(bar.high),
                "low": reconcile_live.wire_decimal(bar.low),
                "close": reconcile_live.wire_decimal(bar.close),
                "volume": Decimal(bar.volume),
                "open_interest": (
                    reconcile_live.wire_decimal(bar.open_interest)
                    if bar.HasField("open_interest")
                    else None
                ),
                "roll_ratio": (
                    reconcile_live.wire_decimal(bar.roll_ratio)
                    if bar.HasField("roll_ratio")
                    else None
                ),
                "roll_yield": (
                    reconcile_live.wire_decimal(bar.roll_yield)
                    if bar.HasField("roll_yield")
                    else None
                ),
            }
        if decision is not None:
            targets = {}
            mode = None
            for target in decision.targets:
                native = f"{target.instrument.exchange}/{target.instrument.symbol}"
                symbol = native_to_symbol.get(native) or reconcile_live.canonical_contract(
                    target.instrument.exchange, target.instrument.symbol, step_day
                )
                size_case = target.WhichOneof("size")
                if size_case == "lots":
                    mode = "lots"
                    targets[symbol] = {"mode": "lots", "value": Decimal(target.lots)}
                elif size_case == "weight":
                    mode = "weight"
                    targets[symbol] = {"mode": "weight", "value": reconcile_live.wire_decimal(target.weight)}
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
    recorded_start_ms = max(window_first_ms, execute_from_ms)
    recorded_contracts = {
        bar["contract"] for (ms, _), bar in live_step_bars.items() if ms >= recorded_start_ms
    }
    for end_ms, native, bar in recorded_raw:
        if not recorded_start_ms <= end_ms <= window_last_ms:
            continue
        contract = native_to_contract.get(native) or reconcile_live.canonical_contract(
            bar.instrument.exchange, bar.instrument.symbol, bar.trading_day
        )
        recorded_contracts.add(contract)
        live_recorded_bars[(end_ms, contract)] = {
            "trading_day": bar.trading_day,
            "open": reconcile_live.wire_decimal(bar.open),
            "high": reconcile_live.wire_decimal(bar.high),
            "low": reconcile_live.wire_decimal(bar.low),
            "close": reconcile_live.wire_decimal(bar.close),
            "volume": Decimal(bar.volume),
            "open_interest": (
                reconcile_live.wire_decimal(bar.open_interest)
                if bar.HasField("open_interest")
                else None
            ),
        }

    return {
        "step_bars": live_step_bars,
        "decisions": live_decisions,
        "recorded_bars": live_recorded_bars,
        "recorded_contracts": recorded_contracts,
        "recorded_start_ms": recorded_start_ms,
        "first_ms": window_first_ms,
        "last_ms": window_last_ms,
        "begin_day": window_begin_day,
        "end_day": window_end_day,
        "execute_from_ms": execute_from_ms,
        "fill_begin_day": max(window_begin_day, first_live.trading_day) if first_live else window_begin_day,
        "warmup_only": window_last_ms < execute_from_ms,
    }


def _resolve_backtest_spec(
    lab_directory: Path,
    instance: Instance,
    *,
    symbols: list[str],
    minutes: int,
    first_step_day: str,
    last_step_day: str,
    trading_account: Path | None,
    run_id: str,
    dataset_version: str,
    strategy_source: Path | None,
    fallback_source: Path | None,
    deposit: Decimal,
):
    if run_id:
        runs = Runs(lab_directory)
        record = runs.record(run_id)
        data_rec = record["data"]
        version_id = dataset_version or data_rec["dataset_version"]
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
            symbols=tuple(data_rec["symbols"]),
            begin_day=first_step_day,
            end_day=last_step_day,
            deposit=Decimal(costs["deposit"]),
            costs=None,
            slippage_ticks=int(costs["slippage_ticks"]),
            fee_multiplier=Decimal(str(costs.get("fee_multiplier", "1"))),
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
        source_dir = strategy_source or _frozen_source(lab_directory, runs, run_id) or fallback_source
        if source_dir is not None:
            strategy = repository.load(source_dir, config.strategy, config.params)
        elif trading_account is not None:
            pkg_dir = (
                trading_account / "strategies" / "packages"
                / instance.package_name / instance.package_version
            )
            strategy = reconcile_live.load_package_strategy(pkg_dir, config.params)
        else:
            raise ValueError(f"strategy source for run {run_id} is unavailable")
        return config, strategy

    version_id = dataset_version or (instance.warmup.dataset_version if instance.HasField("warmup") else "")
    if not version_id:
        raise ValueError("dataset_version is required when --run-id is omitted and instance has no warmup")
    params = reconcile_live.instance_params(instance)
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
        strategy = reconcile_live.load_package_strategy(pkg_dir, params)
    else:
        raise ValueError("strategy repository or run_id is required")
    return config, strategy


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
    minutes = reconcile_live.PERIOD_MINUTES.get(instance.period)
    if minutes is None:
        raise ValueError(f"unsupported strategy period: {instance.period}")

    first_step_day = steps[0][1].bars[0].trading_day
    last_step_day = steps[-1][1].bars[0].trading_day
    symbols, native_to_symbol, native_to_contract = reconcile_live.instance_symbol_maps(
        instance, first_step_day
    )
    window = _extract_live_window(
        instance, steps, recorded_raw, native_to_symbol, native_to_contract, begin_day, end_day
    )
    config, strategy = _resolve_backtest_spec(
        lab_directory,
        instance,
        symbols=symbols,
        minutes=minutes,
        first_step_day=first_step_day,
        last_step_day=last_step_day,
        trading_account=trading_account,
        run_id=run_id,
        dataset_version=dataset_version,
        strategy_source=strategy_source,
        fallback_source=fallback_source,
        deposit=deposit,
    )

    store = dataset if dataset is not None else Dataset(lab_directory)
    replayed = reconcile_diff.replay_backtest(
        store, config, strategy, begin_ms=steps[0][1].bars[0].end_ms,
        end_ms=steps[-1][1].bars[0].end_ms,
        execute_from_ms=window["execute_from_ms"]
    )

    # Align backtest step bars & decisions to the live instance window.
    bt_step_bars = {
        key: val for key, val in replayed["bars"].items()
        if window["first_ms"] <= key[0] <= window["last_ms"]
    }
    bt_decisions = {
        end_ms: val for end_ms, val in replayed["decisions"].items()
        if window["first_ms"] <= end_ms <= window["last_ms"]
    }
    bar_summary, bar_diffs = reconcile_diff.compare_bar_maps(
        window["step_bars"], bt_step_bars, price_tol, volume_tol, roll_yield_tol,
        include_series_fields=True,
    )
    bar_diff_times = {item["end_ms"] for item in bar_diffs}
    decision_summary, decision_diffs = reconcile_diff.compare_decisions(
        window["decisions"], bt_decisions, bar_diff_times, weight_tol
    )

    # Compare raw per-contract bars in Trading's `bars.sqlite` against Warehouse.
    live_recorded_bars = window["recorded_bars"]
    wh_recorded = {
        key: val for key, val in reconcile_diff.warehouse_contract_bars(
            replayed["data"], window["recorded_contracts"], minutes, window["begin_day"], window["end_day"]
        ).items()
        if window["recorded_start_ms"] <= key[0] <= window["last_ms"]
    }
    recorded_summary, recorded_diffs = reconcile_diff.compare_bar_maps(
        live_recorded_bars, wh_recorded, price_tol, volume_tol, roll_yield_tol,
        include_series_fields=False,
    )

    live_fills = []
    for fill in live_fills_raw:
        day = fill["trading_day"]
        if not (window["begin_day"] <= day <= window["end_day"]):
            continue
        native = f"{fill['venue']}/{fill['symbol']}"
        contract = native_to_contract.get(native) or reconcile_live.canonical_contract(
            fill["venue"], fill["symbol"], day
        )
        live_fills.append({**fill, "contract": contract})
    fill_window_begin = window["fill_begin_day"]
    bt_fills = []
    for row in replayed["fills"].iter_rows(named=True):
        day = row["trading_day"].isoformat() if hasattr(row["trading_day"], "isoformat") else str(row["trading_day"])
        if not (fill_window_begin <= day <= window["end_day"]):
            continue
        bt_fills.append({
            "trading_day": day,
            "contract": row["contract"],
            "side": row["side"],
            "offset": row["offset"],
            "quantity": Decimal(row["quantity"]),
            "price": Decimal(row["price"]),
        })
    fill_summary, fill_diffs = reconcile_diff.compare_fills(
        live_fills, bt_fills, fill_price_tol, tick_lookup=reconcile_diff.tick_lookup(replayed["contracts"])
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
            "window_begin_day": window["begin_day"],
            "window_end_day": window["end_day"],
            "fill_window_begin": fill_window_begin,
            "warmup_only": window["warmup_only"],
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
    instance_id = instance_id or reconcile_live.default_instance_id(trading_account)
    live_run = reconcile_live.read_live_run(trading_account, instance_id)
    instance = live_run["instance"]
    steps = live_run["steps"]
    if not steps:
        raise ValueError(f"strategy instance {instance_id} recorded no steps")
    first_step_day = steps[0][1].bars[0].trading_day
    _, _, native_to_contract = reconcile_live.instance_symbol_maps(instance, first_step_day)
    recorded_raw = reconcile_live.read_recorded_bars(
        trading_account, instance.period, set(native_to_contract),
        steps[0][1].bars[0].end_ms, steps[-1][1].bars[0].end_ms,
    )
    journal = reconcile_live.read_live_fills(trading_account, instance_id)
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
        side = reconcile_live.SIDE_NAMES.get(fill.side)
        offset = reconcile_live.OFFSET_NAMES.get(fill.offset)
        qty = reconcile_live.wire_decimal(fill.lots)
        price = reconcile_live.wire_decimal(fill.price)
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
    deposit = reconcile_live.wire_decimal(request.deposit) if request.HasField("deposit") else None
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
        f"实盘 ↔ 回测对账结果: {'通过' if report['ok'] else '发现差异'}"
        + ("（仅预热）" if meta.get("warmup_only") else ""),
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

    parser = argparse.ArgumentParser(
        description="Reconcile Trading recorded bars/decisions/fills vs Lab backtest"
    )
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
