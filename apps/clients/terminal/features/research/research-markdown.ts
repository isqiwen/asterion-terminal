// Markdown summary builders copied from backtest, factor and live reconciliation views.
import type { FactorResult, RunDetail } from "../../src/generated/asterion/lab_pb";
import { runName } from "./research-lab";

const fmtPct = (v: number | undefined | null) =>
  v === undefined || v === null || !Number.isFinite(v) ? "—" : `${(v * 100).toFixed(2)}%`;
const fmtNum = (v: number | undefined | null, digits = 3) =>
  v === undefined || v === null || !Number.isFinite(v) ? "—" : v.toFixed(digits);

export function formatRunSummaryMarkdown(
  detail: RunDetail,
  record: Record<string, unknown>,
  note?: string,
) {
  const s = detail.summary!;
  const rec = record as {
    strategy?: { id?: string; commit?: string };
    params?: Record<string, unknown>;
    costs?: { deposit?: string; slippage_ticks?: number; fee_multiplier?: string | number };
    data?: {
      dataset_version?: string;
      symbols?: string[];
      begin_day?: string;
      end_day?: string;
      bars?: { period?: string };
      diagnostics?: {
        trades?: {
          count?: number;
          win_rate?: number | null;
          payoff_ratio?: number | null;
          profit_factor?: number | null;
          avg_holding_days?: number | null;
        };
        costs?: {
          gross_pnl?: number;
          fees?: number;
          estimated_slippage?: number;
          net_pnl?: number;
          annual_turnover?: number | null;
        };
        exposure?: {
          peak_margin_ratio?: number | null;
          peak_gross_leverage?: number | null;
          avg_net_exposure?: number | null;
          max_drawdown_days?: number;
        };
        benchmark?: {
          total_return?: number | null;
          max_drawdown?: number | null;
          sharpe?: number | null;
        };
        by_symbol?: Array<{ symbol: string; net_pnl: number; fills: number }>;
        trials?: { strategy_runs?: number; holdout_runs?: number };
      };
    };
  };
  const diag = rec.data?.diagnostics;
  const lines = [
    `## Backtest: ${runName(s)} (${s.strategy})`,
    `- Symbols: ${(rec.data?.symbols ?? s.symbols).join(", ")}`,
    `- Period: ${rec.data?.begin_day ?? s.firstDay} to ${rec.data?.end_day ?? s.lastDay} (${rec.data?.bars?.period ?? "day"})`,
    `- Params: ${JSON.stringify(rec.params ?? {})}`,
    `- Deposit: ${rec.costs?.deposit ?? "—"}, Slippage: ${rec.costs?.slippage_ticks ?? 0} ticks`,
    `- Dataset: ${(rec.data?.dataset_version ?? "").slice(0, 12)}${rec.strategy?.commit ? ` · Commit: ${rec.strategy.commit.slice(0, 12)}` : ""}`,
  ];
  if (note) lines.push(`- Note: ${note}`);
  lines.push(
    "",
    "### Core Metrics",
    `| Total Return | Annual Return | Sharpe | Max Drawdown | Calmar | Volatility | Win Days | Fills |`,
    `| --- | --- | --- | --- | --- | --- | --- | --- |`,
    `| ${fmtPct(s.metrics.total_return)} | ${fmtPct(s.metrics.annual_return)} | ${fmtNum(s.metrics.sharpe)} | ${fmtPct(s.metrics.max_drawdown)} | ${fmtNum(s.metrics.calmar)} | ${fmtPct(s.metrics.annual_volatility)} | ${fmtPct(s.metrics.winning_days)} | ${s.metrics.fills ?? 0} |`,
  );
  if (s.metrics.holdout_sharpe !== undefined || s.metrics.holdout_total_return !== undefined) {
    lines.push(
      `- Holdout: return ${fmtPct(s.metrics.holdout_total_return)}, sharpe ${fmtNum(s.metrics.holdout_sharpe)}, drawdown ${fmtPct(s.metrics.holdout_max_drawdown)} (dev sharpe ${fmtNum(s.metrics.development_sharpe)})`,
    );
  }
  if (diag?.benchmark) {
    lines.push(
      `- Equal-Weight Benchmark: return ${fmtPct(diag.benchmark.total_return)}, sharpe ${fmtNum(diag.benchmark.sharpe)}, drawdown ${fmtPct(diag.benchmark.max_drawdown)}`,
    );
  }
  if (diag) {
    lines.push(
      "",
      "### Diagnostics",
      `- Trades: ${diag.trades?.count ?? 0}, win rate ${fmtPct(diag.trades?.win_rate)}, payoff ${fmtNum(diag.trades?.payoff_ratio, 2)}, profit factor ${fmtNum(diag.trades?.profit_factor, 2)}, avg hold ${fmtNum(diag.trades?.avg_holding_days, 1)}d`,
      `- Costs: gross ${fmtNum(diag.costs?.gross_pnl, 0)}, fees ${fmtNum(diag.costs?.fees, 0)}, slippage ${fmtNum(diag.costs?.estimated_slippage, 0)}, net ${fmtNum(diag.costs?.net_pnl, 0)}, turnover ${fmtNum(diag.costs?.annual_turnover, 2)}x`,
      `- Exposure: peak margin ${fmtPct(diag.exposure?.peak_margin_ratio)}, peak leverage ${fmtNum(diag.exposure?.peak_gross_leverage, 2)}x, avg net ${fmtPct(diag.exposure?.avg_net_exposure)}, max underwater ${diag.exposure?.max_drawdown_days ?? 0}d`,
    );
    if (diag.trials?.strategy_runs) {
      lines.push(
        `- Trials: ${diag.trials.strategy_runs} runs for strategy${diag.trials.holdout_runs ? ` (${diag.trials.holdout_runs} on same holdout)` : ""}`,
      );
    }
    if (diag.by_symbol?.length) {
      lines.push(
        `- Top Symbols: ${diag.by_symbol
          .slice(0, 6)
          .map(item => `${item.symbol}: ${item.net_pnl.toFixed(0)} (${item.fills} fills)`)
          .join(", ")}`,
      );
    }
  }
  return lines.join("\n");
}

export function formatFactorSummaryMarkdown(result: FactorResult, note?: string) {
  const ev = result.evaluation;
  const title =
    result.factorNames.length >= 2 ? result.factorNames.join(" + ") : (ev?.factor ?? result.id);
  const lines = [
    `## Factor Evaluation: ${title}`,
    `- Symbols (${result.symbols.length}): ${result.symbols.slice(0, 12).join(", ")}${result.symbols.length > 12 ? ", ..." : ""}`,
    `- Range: ${ev?.firstDay ?? ""} to ${ev?.lastDay ?? ""}`,
    `- Hygiene: return=${ev?.returnMode || "close"}, winsorize=${Boolean(ev?.winsorize)}, neutralize=${ev?.neutralize || "none"}, vol_adjust=${Boolean(ev?.volAdjust)}, cost=${ev?.costBps ?? 0}bps`,
    `- Dataset: ${(result.pinned?.datasetVersion ?? "").slice(0, 12)}`,
  ];
  if (note) lines.push(`- Note: ${note}`);
  if (result.horizons.length) {
    lines.push(
      "",
      "### IC Decay",
      `| Horizon | Mean IC | Std | ICIR | t-stat | Positive | Days |`,
      `| --- | --- | --- | --- | --- | --- | --- |`,
      ...result.horizons.map(
        h =>
          `| ${h.horizon} | ${fmtNum(h.mean)} | ${fmtNum(h.deviation)} | ${fmtNum(h.ir)} | ${fmtNum(h.t)} | ${fmtPct(h.positiveShare)} | ${h.days} |`,
      ),
    );
  }
  if (result.groups.length) {
    lines.push(
      "",
      "### Groups",
      ...result.groups.map(
        g => `- G${g.group}: mean ${fmtPct(g.meanReturn)}, turnover ${fmtPct(g.turnover)}`,
      ),
    );
  }
  return lines.join("\n");
}

export function formatReconcileSummaryMarkdown(report: {
  ok: boolean;
  metadata: {
    instance_id: string;
    package: string;
    symbols: string[];
    window_begin_day: string;
    window_end_day: string;
    period: string;
    dataset_version: string;
    warmup_only: boolean;
  };
  summary: {
    bars: { matched: number; missing_in_live: number; extra_in_live: number; mismatched: number };
    recorded_bars: {
      matched: number;
      missing_in_live: number;
      extra_in_live: number;
      mismatched: number;
    };
    decisions: {
      matched: number;
      missing_in_live: number;
      extra_in_live: number;
      mismatched: number;
    };
    fills: {
      live_fill_count: number;
      backtest_fill_count: number;
      matched: number;
      slippage_samples: number;
      mean_slippage_ticks: number | null;
      median_slippage_ticks: number | null;
      p90_slippage_ticks: number | null;
      max_slippage_ticks: number | null;
    };
  };
}) {
  const { metadata: m, summary: s } = report;
  const ratio = (x: {
    matched: number;
    missing_in_live: number;
    extra_in_live: number;
    mismatched: number;
  }) => {
    const total = x.matched + x.missing_in_live + x.extra_in_live + x.mismatched;
    return total ? `${((x.matched / total) * 100).toFixed(2)}% (${x.matched}/${total})` : "—";
  };
  return [
    `## Live Reconciliation: ${m.package} (${report.ok ? "PASS" : "DIFF"}${m.warmup_only ? ", warmup only" : ""})`,
    `- Instance: ${m.instance_id} · Symbols: ${m.symbols.join(", ")} · ${m.window_begin_day} to ${m.window_end_day} (${m.period})`,
    `- Dataset: ${m.dataset_version.slice(0, 12)}`,
    `- Input Bars Match: ${ratio(s.bars)}`,
    `- Recorded Bars Match: ${ratio(s.recorded_bars)}`,
    `- Decisions Match: ${ratio(s.decisions)}`,
    `- Fills: live ${s.fills.live_fill_count}, backtest ${s.fills.backtest_fill_count}, matched ${s.fills.matched}`,
    `- Adverse Slippage (ticks, n=${s.fills.slippage_samples}): mean ${fmtNum(s.fills.mean_slippage_ticks, 2)}, median ${fmtNum(s.fills.median_slippage_ticks, 2)}, P90 ${fmtNum(s.fills.p90_slippage_ticks, 2)}, max ${fmtNum(s.fills.max_slippage_ticks, 2)}`,
  ].join("\n");
}
