"""Automated post-close Warehouse quality audit and Trading reconciliation runner.

Combines `asterion_lab.audit` (Warehouse fixed-version data checks) and
`asterion_lab.reconcile` (live Trading <-> Lab backtest reconciliation across
one or more accounts) into one CLI suitable for a daily post-close systemd timer,
with deduplicated JSON Webhook alerting compatible with Trading's `alert_webhook`.
"""
import argparse
import datetime
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

from asterion_lab import audit, reconcile
from asterion_lab.warehouse_client import Dataset, Endpoint


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect())


def validate_webhook_url(url: str) -> str:
    """Validate an alert webhook URL using the same rules as Trading's alert_webhook."""
    cleaned = url.strip()
    if not cleaned or len(cleaned) > 1024 or any(c == "\0" or c.isspace() for c in cleaned):
        raise ValueError("invalid alert webhook URL")
    if cleaned.startswith("https://"):
        scheme_end = 8
    elif cleaned.startswith("http://"):
        scheme_end = 7
    else:
        raise ValueError("alert webhook URL must start with https:// or http://")
    slash = cleaned.find("/", scheme_end)
    authority = cleaned[scheme_end:] if slash == -1 else cleaned[scheme_end:slash]
    if not authority or authority.startswith(":") or authority.endswith(":"):
        raise ValueError("alert webhook URL needs a host")
    return cleaned


def resolve_webhook_url(webhook_url: str = "", webhook_file: Path | None = None) -> str:
    """Resolve webhook URL from explicit arg, file, systemd credential, or env var."""
    if webhook_url:
        return validate_webhook_url(webhook_url)
    if webhook_file is not None:
        return validate_webhook_url(webhook_file.read_text(encoding="utf-8"))
    cred_dir = os.environ.get("CREDENTIALS_DIRECTORY", "").strip()
    if cred_dir:
        cred_path = Path(cred_dir) / "alert_webhook_url"
        if cred_path.is_file():
            return validate_webhook_url(cred_path.read_text(encoding="utf-8"))
    env_url = os.environ.get("ASTERION_WEBHOOK_URL", "").strip()
    if env_url:
        return validate_webhook_url(env_url)
    return ""


def send_webhook(
    url: str,
    payload: dict,
    *,
    timeout: float = 5.0,
    retry_delays: tuple[float, ...] = (0.5, 1.5, 3.5),
) -> int:
    """POST JSON payload to `url` with bounded retries on 429/5xx/network errors.

    Never embeds `url` in returned errors so secret webhook tokens stay out of logs.
    """
    target = validate_webhook_url(url)
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    max_attempts = 1 + len(retry_delays)
    last_error = "unknown error"
    for attempt in range(max_attempts):
        req = urllib.request.Request(
            target,
            data=body,
            headers={"Content-Type": "application/json; charset=utf-8"},
            method="POST",
        )
        retryable = False
        try:
            with _OPENER.open(req, timeout=timeout) as resp:
                status = int(resp.status)
                if 200 <= status < 300:
                    return status
                last_error = f"HTTP {status}"
                retryable = status == 429 or status >= 500
        except urllib.error.HTTPError as err:
            status = int(err.code)
            last_error = f"HTTP {status}"
            retryable = status == 429 or status >= 500
        except (urllib.error.URLError, TimeoutError, OSError) as err:
            last_error = f"connection error ({type(err).__name__})"
            retryable = True
        if not retryable or attempt + 1 >= max_attempts:
            break
        time.sleep(retry_delays[attempt])
    raise RuntimeError(f"alert webhook delivery failed: {last_error}")


def parse_account_spec(spec: str) -> tuple[Path, str, str]:
    """Parse `<account_dir>[::<instance_id>[::<run_id>]]`."""
    parts = spec.split("::")
    if len(parts) > 3 or not parts[0].strip():
        raise ValueError(f"invalid --trading-account spec: {spec!r}")
    path = Path(parts[0].strip())
    instance_id = parts[1].strip() if len(parts) >= 2 else ""
    run_id = parts[2].strip() if len(parts) == 3 else ""
    return path, instance_id, run_id


def _normalize_lab_root(lab_dir: Path) -> Path:
    if lab_dir.name == Dataset.provider and lab_dir.parent.name == "warehouse-cache":
        return lab_dir.parents[1]
    return lab_dir


def _alert_digest(audit_report: dict | None, account_results: list[dict]) -> str:
    fingerprint = {
        "audit": None if audit_report is None else {
            "ok": audit_report.get("ok"),
            "dataset_version": audit_report.get("dataset_version", ""),
            "summary": audit_report.get("summary"),
            "error": audit_report.get("error", ""),
        },
        "accounts": [
            {
                "account": item["account"],
                "ok": item["ok"],
                "skipped": item.get("skipped", False),
                "instance_id": item.get("instance_id", ""),
                "summary": item.get("summary"),
                "error": item.get("error", ""),
            }
            for item in account_results
        ],
    }
    raw = json.dumps(fingerprint, sort_keys=True, ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _read_state_digest(state_file: Path | None) -> str:
    if state_file is None or not state_file.is_file():
        return ""
    try:
        data = json.loads(state_file.read_text(encoding="utf-8"))
        return str(data.get("last_alert_digest", ""))
    except (OSError, ValueError):
        return ""


def _write_state_digest(state_file: Path | None, digest: str):
    if state_file is None:
        return
    state_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = state_file.with_name(f".{state_file.name}.tmp.{os.getpid()}")
    tmp.write_text(json.dumps({"last_alert_digest": digest}, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, state_file)


def _build_headline_and_details(audit_report: dict | None, account_results: list[dict]) -> tuple[str, dict]:
    issues = []
    details: dict = {}
    if audit_report is not None:
        if audit_report.get("error"):
            issues.append(f"数据自检失败({audit_report['error']})")
            details["audit"] = {"ok": False, "error": audit_report["error"]}
        else:
            s = audit_report["summary"]
            details["audit"] = {
                "ok": audit_report["ok"],
                "dataset_version": audit_report["dataset_version"][:12],
                "summary": s,
            }
            if not audit_report["ok"]:
                parts = []
                if s["missing_daily_days"] or s["missing_settlement_days"] or s["missing_minute_days"]:
                    parts.append(
                        f"缺日线{s['missing_daily_days']}/结算{s['missing_settlement_days']}/分钟{s['missing_minute_days']}"
                    )
                if s["volume_mismatches"]:
                    parts.append(f"量差{s['volume_mismatches']}")
                if s["price_anomalies"] or s["open_interest_anomalies"]:
                    parts.append(f"价格异常{s['price_anomalies']}/持仓异常{s['open_interest_anomalies']}")
                issues.append("数据质量异常: " + ", ".join(parts))

    acct_details = []
    for item in account_results:
        entry = {"account": item["account"], "ok": item["ok"]}
        if item.get("skipped"):
            entry["skipped"] = True
            entry["reason"] = item.get("reason", "")
        elif item.get("error"):
            entry["error"] = item["error"]
            issues.append(f"账户 {item['account']} 对账失败({item['error']})")
        else:
            s = item["summary"]
            entry["instance_id"] = item["instance_id"]
            entry["bars_ok"] = s["bars"]["ok"]
            entry["decisions_ok"] = s["decisions"]["ok"]
            entry["fills_ok"] = s["fills"]["ok"]
            if s["fills"].get("mean_slippage_ticks") is not None:
                entry["mean_slippage_ticks"] = s["fills"]["mean_slippage_ticks"]
                entry["p90_slippage_ticks"] = s["fills"]["p90_slippage_ticks"]
            if not item["ok"]:
                diffs = []
                if not s["bars"]["ok"] or not s["recorded_bars"]["ok"]:
                    diffs.append("K线差异")
                if not s["decisions"]["ok"]:
                    diffs.append("决策差异")
                if not s["fills"]["ok"]:
                    diffs.append("成交/滑点差异")
                issues.append(f"账户 {item['account']}({item['instance_id']}) 对账异常: " + ", ".join(diffs))
        acct_details.append(entry)
    if acct_details:
        details["accounts"] = acct_details

    headline = "；".join(issues) if issues else "Warehouse 数据与全部账户对账正常"
    return headline, details


def run_inspection(
    *,
    warehouse_dir: Path | None = None,
    lab_dir: Path | None = None,
    terminal_data: Path | None = None,
    provider: str = "research",
    version_id: str = "",
    exchanges: set[str] | None = None,
    begin_day: str = "",
    end_day: str = "",
    recent_days: int = 0,
    skip_audit: bool = False,
    volume_tol: Decimal = Decimal(0),
    volume_rel_tol: Decimal = Decimal(0),
    max_price_jump: Decimal = Decimal("0.25"),
    max_minute_jump: Decimal = Decimal("0.15"),
    oi_jump_ratio: Decimal = Decimal("5"),
    min_oi: Decimal = Decimal("100"),
    accounts: list[tuple[Path, str, str]] | None = None,
    strategy_source: Path | None = None,
    allow_no_instance: bool = False,
    deposit: Decimal = Decimal("1000000"),
    price_tol: Decimal = Decimal(0),
    reconcile_volume_tol: Decimal = Decimal(0),
    weight_tol: Decimal = Decimal("0.000001"),
    fill_price_tol: Decimal = Decimal(0),
    webhook_url: str = "",
    always_notify: bool = False,
    state_file: Path | None = None,
    today: datetime.date | None = None,
    webhook_retry_delays: tuple[float, ...] = (0.5, 1.5, 3.5),
) -> dict:
    """Execute post-close Warehouse audit, Trading reconciliation, and optional alert push."""
    account_list = list(accounts or ())
    if skip_audit and not account_list:
        raise ValueError("nothing to inspect: enable audit or specify at least one --trading-account")
    if not skip_audit and warehouse_dir is None and lab_dir is None:
        raise ValueError("audit requires --warehouse-dir or --lab-dir")
    if account_list and lab_dir is None:
        raise ValueError("--lab-dir is required when --trading-account is specified")
    if recent_days < 0:
        raise ValueError("--recent-days must be nonnegative")

    eff_begin = begin_day
    if not eff_begin and recent_days > 0:
        ref_day = today or datetime.date.today()
        eff_begin = (ref_day - datetime.timedelta(days=recent_days - 1)).isoformat()

    endpoint = Endpoint(terminal_data) if terminal_data is not None else None
    lab_root = _normalize_lab_root(lab_dir) if lab_dir is not None else None

    audit_report = None
    if not skip_audit:
        try:
            if warehouse_dir is not None:
                store = audit.LocalWarehouse(warehouse_dir, provider=provider)
            else:
                store = Dataset(lab_root, history=endpoint)
            audit_report = audit.audit(
                store,
                version_id=version_id,
                exchanges=exchanges,
                begin_day=eff_begin,
                end_day=end_day,
                volume_tol=volume_tol,
                volume_rel_tol=volume_rel_tol,
                max_price_jump=max_price_jump,
                max_minute_jump=max_minute_jump,
                oi_jump_ratio=oi_jump_ratio,
                min_oi=min_oi,
            )
        except Exception as err:
            audit_report = {"ok": False, "error": str(err)}

    account_results = []
    for account_path, instance_id, run_id in account_list:
        account_name = account_path.name or str(account_path)
        try:
            dataset = Dataset(lab_root, history=endpoint) if endpoint is not None else None
            rec = reconcile.reconcile(
                account_path,
                lab_root,
                instance_id=instance_id,
                run_id=run_id,
                dataset_version=version_id,
                strategy_source=strategy_source,
                dataset=dataset,
                begin_day=eff_begin,
                end_day=end_day,
                deposit=deposit,
                price_tol=price_tol,
                volume_tol=reconcile_volume_tol,
                weight_tol=weight_tol,
                fill_price_tol=fill_price_tol,
            )
            account_results.append({
                "account": account_name,
                "path": str(account_path),
                "instance_id": rec["metadata"]["instance_id"],
                "ok": rec["ok"],
                "metadata": rec["metadata"],
                "summary": rec["summary"],
                "report": rec,
            })
        except FileNotFoundError as err:
            if allow_no_instance and not instance_id and "instance.pb" in str(err):
                account_results.append({
                    "account": account_name,
                    "path": str(account_path),
                    "ok": True,
                    "skipped": True,
                    "reason": "no_instance",
                })
            else:
                account_results.append({
                    "account": account_name,
                    "path": str(account_path),
                    "ok": False,
                    "error": str(err),
                })
        except Exception as err:
            account_results.append({
                "account": account_name,
                "path": str(account_path),
                "ok": False,
                "error": str(err),
            })

    overall_ok = (audit_report is None or bool(audit_report["ok"])) and all(
        item["ok"] for item in account_results
    )
    digest = _alert_digest(audit_report, account_results)
    headline, details = _build_headline_and_details(audit_report, account_results)

    webhook_info = {"status": "disabled"}
    if webhook_url:
        validated_url = validate_webhook_url(webhook_url)
        should_send = always_notify or not overall_ok
        if overall_ok:
            _write_state_digest(state_file, "")
        if should_send:
            prev_digest = _read_state_digest(state_file)
            if not overall_ok and not always_notify and prev_digest == digest:
                webhook_info = {"status": "deduplicated", "digest": digest}
            else:
                kind = "post_close_ok" if overall_ok else "post_close_alert"
                account_label = (
                    ",".join(item["account"] for item in account_results)
                    if account_results else "warehouse"
                )
                payload = {
                    "service": "asterion-post-close",
                    "account": account_label,
                    "kind": kind,
                    "time_ms": int(time.time() * 1000),
                    "text": f"[Asterion {account_label}] {kind}: {headline}",
                    "details": details,
                }
                try:
                    http_status = send_webhook(
                        validated_url, payload, retry_delays=webhook_retry_delays
                    )
                    webhook_info = {"status": "sent", "http_status": http_status, "kind": kind}
                    if not overall_ok:
                        _write_state_digest(state_file, digest)
                except Exception as err:
                    webhook_info = {"status": "failed", "error": str(err), "kind": kind}
        else:
            webhook_info = {"status": "skipped_ok"}
    elif overall_ok:
        _write_state_digest(state_file, "")

    return {
        "ok": overall_ok,
        "time_ms": int(time.time() * 1000),
        "begin_day": eff_begin,
        "end_day": end_day,
        "headline": headline,
        "alert_digest": digest,
        "audit": audit_report,
        "accounts": account_results,
        "webhook": webhook_info,
    }


def format_report(report: dict) -> str:
    lines = [
        f"盘后自动巡检: {'通过' if report['ok'] else '发现异常'}",
        f"  摘要: {report['headline']}",
    ]
    if report["begin_day"] or report["end_day"]:
        lines.append(f"  区间: {report['begin_day'] or '最早'} ~ {report['end_day'] or '最新'}")
    if report["audit"] is not None:
        lines.append("")
        if report["audit"].get("error"):
            lines.append(f"Warehouse 数据质量自检失败: {report['audit']['error']}")
        else:
            lines.append(audit.format_report(report["audit"]))
    for item in report["accounts"]:
        lines.append("")
        if item.get("skipped"):
            lines.append(f"账户 {item['account']}: 跳过 ({item.get('reason', '')})")
        elif item.get("error"):
            lines.append(f"账户 {item['account']} 对账失败: {item['error']}")
        else:
            lines.append(f"账户 {item['account']}:")
            lines.append(reconcile.format_report(item["report"]))
    wh = report["webhook"]
    if wh["status"] != "disabled":
        lines.append("")
        lines.append(f"Webhook 通知: {wh['status']}" + (f" ({wh['error']})" if wh.get("error") else ""))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Automated post-close Warehouse audit + Trading reconciliation + Webhook alert"
    )
    parser.add_argument("--warehouse-dir", type=Path, default=None, help="Local Warehouse data directory")
    parser.add_argument("--lab-dir", type=Path, default=None, help="Lab data directory (runs/ & warehouse-cache/)")
    parser.add_argument("--terminal-data", type=Path, default=None, help="Terminal data dir for remote fetch")
    parser.add_argument("--provider", default="research", help="Warehouse provider store (default: research)")
    parser.add_argument("--version", default="", help="Fixed Warehouse dataset version SHA-256")
    parser.add_argument("--exchange", action="append", default=[], help="Filter audit to exchange(s), repeatable")
    parser.add_argument("--begin-day", default="", help="Start day YYYY-MM-DD")
    parser.add_argument("--end-day", default="", help="End day YYYY-MM-DD")
    parser.add_argument("--recent-days", type=int, default=0, help="Inspect the last N calendar days if --begin-day omitted")
    parser.add_argument("--skip-audit", action="store_true", help="Skip Warehouse data quality audit")
    parser.add_argument("--volume-tol", default="0", help="Allowed absolute minute-vs-daily volume diff")
    parser.add_argument("--volume-rel-tol", default="0", help="Allowed relative minute-vs-daily volume diff")
    parser.add_argument("--max-price-jump", default="0.25", help="Daily close jump threshold")
    parser.add_argument("--max-minute-jump", default="0.15", help="Intraday minute jump threshold")
    parser.add_argument("--oi-jump-ratio", default="5", help="Daily open-interest jump ratio threshold")
    parser.add_argument("--min-oi", default="100", help="Minimum previous OI for jump checks")
    parser.add_argument(
        "--trading-account",
        action="append",
        default=[],
        help="Trading account spec <dir>[::<instance_id>[::<run_id>]], repeatable",
    )
    parser.add_argument("--strategy-source", type=Path, default=None, help="Strategy repository/snapshot directory")
    parser.add_argument("--allow-no-instance", action="store_true", help="Skip accounts without instance.pb")
    parser.add_argument("--deposit", default="1000000", help="Deposit when replaying without run_id")
    parser.add_argument("--price-tol", default="0", help="Allowed reconcile OHLC price delta")
    parser.add_argument("--reconcile-volume-tol", default="0", help="Allowed reconcile volume/OI delta")
    parser.add_argument("--weight-tol", default="0.000001", help="Allowed reconcile target weight delta")
    parser.add_argument("--fill-price-tol", default="0", help="Allowed reconcile fill VWAP slippage delta")
    parser.add_argument("--webhook-url", default="", help="Alert webhook URL (prefer --webhook-file or systemd credential)")
    parser.add_argument("--webhook-file", type=Path, default=None, help="File containing the alert webhook URL")
    parser.add_argument("--always-notify", action="store_true", help="Send webhook summary even when all checks pass")
    parser.add_argument("--state-file", type=Path, default=None, help="State file for deduplicating identical alerts")
    parser.add_argument("--output", type=Path, default=None, help="Write full JSON report to this path")
    args = parser.parse_args(argv)

    webhook_url = resolve_webhook_url(args.webhook_url, args.webhook_file)
    accounts = [parse_account_spec(spec) for spec in args.trading_account]
    report = run_inspection(
        warehouse_dir=args.warehouse_dir,
        lab_dir=args.lab_dir,
        terminal_data=args.terminal_data,
        provider=args.provider,
        version_id=args.version,
        exchanges=set(args.exchange) if args.exchange else None,
        begin_day=args.begin_day,
        end_day=args.end_day,
        recent_days=args.recent_days,
        skip_audit=args.skip_audit,
        volume_tol=Decimal(args.volume_tol),
        volume_rel_tol=Decimal(args.volume_rel_tol),
        max_price_jump=Decimal(args.max_price_jump),
        max_minute_jump=Decimal(args.max_minute_jump),
        oi_jump_ratio=Decimal(args.oi_jump_ratio),
        min_oi=Decimal(args.min_oi),
        accounts=accounts,
        strategy_source=args.strategy_source,
        allow_no_instance=args.allow_no_instance,
        deposit=Decimal(args.deposit),
        price_tol=Decimal(args.price_tol),
        reconcile_volume_tol=Decimal(args.reconcile_volume_tol),
        weight_tol=Decimal(args.weight_tol),
        fill_price_tol=Decimal(args.fill_price_tol),
        webhook_url=webhook_url,
        always_notify=args.always_notify,
        state_file=args.state_file,
    )
    print(format_report(report))
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    delivery_failed = report["webhook"]["status"] == "failed"
    sys.exit(0 if (report["ok"] and not delivery_failed) else 1)


if __name__ == "__main__":
    main()
