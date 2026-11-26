// Client-side quantitative research analytics: monthly returns, heatmaps,
// factor IC rollups, DSR, break-even slippage, portfolio comparison and run verdicts.
import type {
  FactorDay,
  FactorResult,
  RunDay,
  RunDetail,
  RunSummary,
} from "../../src/generated/asterion/lab_pb";
import { type MessageValues, translate } from "../../src/i18n";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export const SLIPPAGE_STRESS_TICKS = [0, 1, 2, 3, 5] as const;

// Each month's return from the equity it began with: the last settlement of
// the month before, or the deposit.
export function monthlyReturns(deposit: string, days: readonly RunDay[]) {
  const months: { month: string; value: number }[] = [];
  let opening = Number(deposit);
  for (let i = 0; i < days.length; i++) {
    const month = days[i].tradingDay.slice(0, 7);
    if (i + 1 < days.length && days[i + 1].tradingDay.slice(0, 7) === month) continue;
    const closing = Number(days[i].equity?.text);
    if (opening > 0 && Number.isFinite(closing)) {
      months.push({ month, value: Math.max(-1, closing / opening - 1) });
    }
    opening = closing;
  }
  return months;
}

// A sweep over two parameters as a grid of one metric; null where a
// combination has no value.
export function heatmap(
  runs: readonly { params: Record<string, string>; metric: number | null }[],
  row: string,
  column: string,
) {
  const sorted = (name: string) =>
    [...new Set(runs.map(run => run.params[name]))].sort(
      (a, b) => Number(a) - Number(b) || a.localeCompare(b),
    );
  const rows = sorted(row),
    columns = sorted(column);
  const cells = rows.map(r =>
    columns.map(
      c => runs.find(run => run.params[row] === r && run.params[column] === c)?.metric ?? null,
    ),
  );
  return { rows, columns, cells };
}

// The running sum of the days' IC; a day without one adds nothing.
export function cumulativeIc(days: readonly FactorDay[]) {
  let total = 0;
  return days.map(day => (total += day.ic ?? 0));
}

export function factorTurnover(result: FactorResult | undefined): number | undefined {
  if (!result?.groups.length) return undefined;
  const values = result.groups
    .map(group => group.turnover)
    .filter((value): value is number => value !== undefined && Number.isFinite(value));
  if (!values.length) return undefined;
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

export type YearlyIc = {
  year: string;
  days: number;
  mean: number;
  deviation?: number;
  ir?: number;
  t?: number;
  positiveShare: number;
};

// Yearly IC breakdown matching factor.py::statistics (sample std ddof=1 and overlap-adjusted t).
export function yearlyIc(days: readonly FactorDay[], horizon: number): YearlyIc[] {
  const byYear = new Map<string, number[]>();
  for (const day of days) {
    if (day.ic === undefined || !Number.isFinite(day.ic)) continue;
    const year = day.tradingDay.slice(0, 4);
    if (!year) continue;
    const bucket = byYear.get(year);
    if (bucket) bucket.push(day.ic);
    else byYear.set(year, [day.ic]);
  }
  const effectiveHorizon = Math.max(1, horizon);
  return [...byYear.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([year, values]) => {
      const count = values.length;
      const mean = values.reduce((sum, value) => sum + value, 0) / count;
      const positiveShare = values.filter(value => value > 0).length / count;
      let deviation: number | undefined;
      let ir: number | undefined;
      let tStat: number | undefined;
      if (count > 1) {
        const variance = values.reduce((sum, value) => sum + (value - mean) ** 2, 0) / (count - 1);
        deviation = Math.sqrt(variance);
        if (deviation > 0) {
          ir = mean / deviation;
          tStat = ir * Math.sqrt(count / effectiveHorizon);
        }
      }
      return { year, days: count, mean, deviation, ir, t: tStat, positiveShare };
    });
}

// Standard normal CDF (Abramowitz & Stegun 7.1.26) and Acklam inverse normal CDF.
function normCdf(x: number) {
  const sign = x < 0 ? -1 : 1;
  const z = Math.abs(x) / Math.SQRT2;
  const tVal = 1 / (1 + 0.3275911 * z);
  const poly =
    ((((1.061405429 * tVal - 1.453152027) * tVal + 1.421413741) * tVal - 0.284496736) * tVal +
      0.254829592) *
    tVal;
  const erf = 1 - poly * Math.exp(-z * z);
  return 0.5 * (1 + sign * erf);
}

function normInv(p: number) {
  const clamped = Math.min(1 - 1e-12, Math.max(1e-12, p));
  const a = [
    -3.969683028665376e1, 2.209460984245205e2, -2.759285104469687e2, 1.38357751867269e2,
    -3.066479806614716e1, 2.506628277459239,
  ];
  const b = [
    -5.447609879822406e1, 1.615858368580409e2, -1.556989798598866e2, 6.680131188771972e1,
    -1.328068155288572e1,
  ];
  const c = [
    -7.784894002430293e-3, -3.223964580411365e-1, -2.400758277161838, -2.549732539343734,
    4.374664141464968, 2.938163982698783,
  ];
  const d = [7.784695709041462e-3, 3.224671290700398e-1, 2.445134137142996, 3.754408661907416];
  const pLow = 0.02425;
  if (clamped < pLow) {
    const q = Math.sqrt(-2 * Math.log(clamped));
    return (
      (((((c[0]! * q + c[1]!) * q + c[2]!) * q + c[3]!) * q + c[4]!) * q + c[5]!) /
      ((((d[0]! * q + d[1]!) * q + d[2]!) * q + d[3]!) * q + 1)
    );
  }
  if (clamped > 1 - pLow) {
    const q = Math.sqrt(-2 * Math.log(1 - clamped));
    return (
      -(((((c[0]! * q + c[1]!) * q + c[2]!) * q + c[3]!) * q + c[4]!) * q + c[5]!) /
      ((((d[0]! * q + d[1]!) * q + d[2]!) * q + d[3]!) * q + 1)
    );
  }
  const q = clamped - 0.5;
  const r = q * q;
  return (
    ((((((a[0]! * r + a[1]!) * r + a[2]!) * r + a[3]!) * r + a[4]!) * r + a[5]!) * q) /
    (((((b[0]! * r + b[1]!) * r + b[2]!) * r + b[3]!) * r + b[4]!) * r + 1)
  );
}

// Bailey & Lopez de Prado (2014), "The Deflated Sharpe Ratio: Correcting for Selection Bias,
// Backtest Overfitting, and Non-Normality", JPM 40(5), 94-107, DOI: 10.3905/jpm.2014.40.5.094.
// Adapted for sweep summaries with Gaussian baseline (skew=0, kurtosis=3) and daily periods.
export function deflatedSharpeFromStd(
  bestSharpe: number,
  trials: number,
  sharpeStd: number,
  observations: number,
  periodsPerYear = 252,
): { dsr: number; expectedMax: number; trials: number } | null {
  if (trials < 2 || !Number.isFinite(bestSharpe) || !Number.isFinite(sharpeStd) || observations < 5)
    return null;
  const scale = Math.sqrt(Math.max(1, periodsPerYear));
  // Convert annualized Sharpe ratios to per-period Sharpe before applying Eq. (2) and (8).
  const srStar = bestSharpe / scale;
  const srStd = Math.max(0, sharpeStd) / scale;
  const euler = 0.5772156649015329;
  const sr0 =
    srStd > 0
      ? srStd * ((1 - euler) * normInv(1 - 1 / trials) + euler * normInv(1 - 1 / (trials * Math.E)))
      : 0;
  const denom = Math.sqrt(Math.max(1e-12, 1 + 0.5 * srStar * srStar));
  const z = ((srStar - sr0) * Math.sqrt(observations - 1)) / denom;
  return { dsr: normCdf(z), expectedMax: sr0 * scale, trials };
}

export function deflatedSharpeRatio(
  bestSharpe: number,
  trialSharpes: readonly number[],
  observations: number,
  periodsPerYear = 252,
): { dsr: number; expectedMax: number; trials: number } | null {
  const finite = trialSharpes.filter(Number.isFinite);
  const n = finite.length;
  if (n < 2) return null;
  const mean = finite.reduce((s, v) => s + v, 0) / n;
  const variance = finite.reduce((s, v) => s + (v - mean) ** 2, 0) / (n - 1);
  return deflatedSharpeFromStd(
    bestSharpe,
    n,
    Math.sqrt(Math.max(0, variance)),
    observations,
    periodsPerYear,
  );
}

export function medianValue(values: readonly number[]) {
  const finite = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!finite.length) return null;
  const mid = Math.floor(finite.length / 2);
  return finite.length % 2 === 1 ? finite[mid]! : (finite[mid - 1]! + finite[mid]!) / 2;
}

export function breakEvenSlippage(
  points: readonly { ticks: number; sharpe?: number; totalReturn?: number }[],
): { ticks: number | null; status: "above_max" | "below_zero" | "interpolated"; maxTicks: number } {
  const sorted = [...points].sort((a, b) => a.ticks - b.ticks);
  const maxTicks = sorted.at(-1)?.ticks ?? 0;
  const score = (item: (typeof sorted)[number]) =>
    item.sharpe !== undefined && Number.isFinite(item.sharpe)
      ? item.sharpe
      : item.totalReturn !== undefined && Number.isFinite(item.totalReturn)
        ? item.totalReturn
        : null;
  const valid = sorted
    .map(item => ({ ticks: item.ticks, value: score(item) }))
    .filter((item): item is { ticks: number; value: number } => item.value !== null);
  if (!valid.length) return { ticks: null, status: "below_zero", maxTicks };
  if (valid[0]!.value <= 0) return { ticks: valid[0]!.ticks, status: "below_zero", maxTicks };
  for (let i = 1; i < valid.length; i++) {
    const prev = valid[i - 1]!;
    const cur = valid[i]!;
    if (cur.value <= 0) {
      const span = prev.value - cur.value;
      const frac = span > 0 ? prev.value / span : 0;
      return {
        ticks: prev.ticks + frac * (cur.ticks - prev.ticks),
        status: "interpolated",
        maxTicks,
      };
    }
  }
  return { ticks: null, status: "above_max", maxTicks };
}

export type PortfolioComparison = {
  days: string[];
  correlations: (number | null)[][];
  avgCorrelation: number | null;
  weights: number[];
  nav: number[];
  totalReturn: number;
  annualReturn: number | null;
  annualVolatility: number | null;
  sharpe: number | null;
  maxDrawdown: number;
  volReduction: number | null;
};

// Align compared runs on overlapping trading days to compute correlation and synthetic portfolio.
export function comparePortfolio(
  details: readonly RunDetail[],
  mode: "equal" | "inv_vol",
): PortfolioComparison | null {
  if (details.length < 2) return null;
  const navMaps = details.map(detail => {
    const deposit = Number(JSON.parse(detail.record).costs?.deposit ?? 0);
    const map = new Map<string, number>();
    if (!Number.isFinite(deposit) || deposit <= 0) return map;
    for (const item of detail.days) {
      const eq = Number(item.equity?.text ?? 0);
      if (Number.isFinite(eq) && eq > 0) map.set(item.tradingDay, eq / deposit);
    }
    return map;
  });
  const commonDays = [...(navMaps[0]?.keys() ?? [])].filter(day =>
    navMaps.every(map => map.has(day)),
  );
  if (commonDays.length < 3) return null;

  const seriesReturns = navMaps.map(map => {
    const rets: number[] = [];
    for (let i = 1; i < commonDays.length; i++) {
      const prev = map.get(commonDays[i - 1]!)!;
      const cur = map.get(commonDays[i]!)!;
      rets.push(cur / prev - 1);
    }
    return rets;
  });

  const n = details.length;
  const m = commonDays.length - 1;
  const means = seriesReturns.map(r => r.reduce((s, v) => s + v, 0) / m);
  const stds = seriesReturns.map((r, idx) => {
    const mu = means[idx]!;
    const v = r.reduce((s, x) => s + (x - mu) ** 2, 0) / Math.max(1, m - 1);
    return Math.sqrt(v);
  });

  const correlations: (number | null)[][] = Array.from({ length: n }, () =>
    Array.from({ length: n }, () => null),
  );
  const offDiag: number[] = [];
  for (let i = 0; i < n; i++) {
    correlations[i]![i] = 1;
    for (let j = i + 1; j < n; j++) {
      const denom = stds[i]! * stds[j]!;
      let corr: number | null = null;
      if (denom > 1e-12) {
        let cov = 0;
        for (let tIdx = 0; tIdx < m; tIdx++) {
          cov += (seriesReturns[i]![tIdx]! - means[i]!) * (seriesReturns[j]![tIdx]! - means[j]!);
        }
        cov /= Math.max(1, m - 1);
        corr = Math.max(-1, Math.min(1, cov / denom));
        offDiag.push(corr);
      }
      correlations[i]![j] = corr;
      correlations[j]![i] = corr;
    }
  }
  const avgCorrelation = offDiag.length
    ? offDiag.reduce((s, v) => s + v, 0) / offDiag.length
    : null;

  let weights = Array.from({ length: n }, () => 1 / n);
  if (mode === "inv_vol" && stds.every(s => s > 1e-12)) {
    const inv = stds.map(s => 1 / s);
    const total = inv.reduce((s, v) => s + v, 0);
    weights = inv.map(v => v / total);
  }

  const portReturns: number[] = [];
  const nav: number[] = [1];
  let peak = 1;
  let maxDrawdown = 0;
  for (let tIdx = 0; tIdx < m; tIdx++) {
    let r = 0;
    for (let i = 0; i < n; i++) r += weights[i]! * seriesReturns[i]![tIdx]!;
    portReturns.push(r);
    const nextNav = nav[tIdx]! * (1 + r);
    nav.push(nextNav);
    if (nextNav > peak) peak = nextNav;
    const dd = peak > 0 ? (peak - nextNav) / peak : 0;
    if (dd > maxDrawdown) maxDrawdown = dd;
  }

  const totalReturn = nav.at(-1)! - 1;
  const meanP = portReturns.reduce((s, v) => s + v, 0) / m;
  const varP = portReturns.reduce((s, v) => s + (v - meanP) ** 2, 0) / Math.max(1, m - 1);
  const stdP = Math.sqrt(varP);
  const annualVolatility = stdP * Math.sqrt(252);
  const annualReturn = nav.at(-1)! > 0 ? Math.pow(nav.at(-1)!, 252 / m) - 1 : null;
  const sharpe = stdP > 1e-12 ? (meanP / stdP) * Math.sqrt(252) : null;
  const weightedAvgVol = weights.reduce((s, w, i) => s + w * stds[i]!, 0) * Math.sqrt(252);
  const volReduction =
    weightedAvgVol > 1e-12 ? (weightedAvgVol - annualVolatility) / weightedAvgVol : null;

  return {
    days: commonDays,
    correlations,
    avgCorrelation,
    weights,
    nav,
    totalReturn,
    annualReturn,
    annualVolatility,
    sharpe,
    maxDrawdown,
    volReduction,
  };
}

export type DrawdownEpisode = {
  peak_day: string;
  trough_day: string;
  recovery_day: string | null;
  depth: number;
  peak_to_trough_days: number;
  duration_days: number;
};

export function computeDrawdowns(
  days: readonly RunDay[],
  deposit: number,
  limit = 5,
): DrawdownEpisode[] {
  if (!days.length) return [];
  const equities = days.map(item => Number(item.equity?.text ?? 0));
  let peak = deposit > 0 ? deposit : equities[0]!;
  let peakIdx = deposit > 0 && deposit >= equities[0]! ? -1 : 0;
  let peakDay = days[0]!.tradingDay;
  let trough = peak;
  let troughIdx = peakIdx;
  let troughDay = peakDay;
  const episodes: DrawdownEpisode[] = [];

  for (let idx = 0; idx < days.length; idx++) {
    const eq = equities[idx]!;
    const dayStr = days[idx]!.tradingDay;
    if (eq >= peak) {
      if (peak > 0 && trough < peak) {
        episodes.push({
          peak_day: peakDay,
          trough_day: troughDay,
          recovery_day: dayStr,
          depth: trough / peak - 1,
          peak_to_trough_days: Math.max(1, troughIdx - Math.max(0, peakIdx)),
          duration_days: Math.max(1, idx - Math.max(0, peakIdx)),
        });
      }
      peak = eq;
      peakIdx = idx;
      peakDay = dayStr;
      trough = eq;
      troughIdx = idx;
      troughDay = dayStr;
    } else if (eq < trough) {
      trough = eq;
      troughIdx = idx;
      troughDay = dayStr;
    }
  }
  if (peak > 0 && trough < peak) {
    episodes.push({
      peak_day: peakDay,
      trough_day: troughDay,
      recovery_day: null,
      depth: trough / peak - 1,
      peak_to_trough_days: Math.max(1, troughIdx - Math.max(0, peakIdx)),
      duration_days: Math.max(1, days.length - 1 - Math.max(0, peakIdx)),
    });
  }
  return episodes.sort((a, b) => a.depth - b.depth).slice(0, limit);
}

export type ReturnBin = { from: number; to: number; count: number; center: number };

export function returnHistogram(returns: readonly number[], bucketCount = 17) {
  const valid = returns.filter(value => Number.isFinite(value));
  if (valid.length < 3) return null;
  const sorted = [...valid].sort((a, b) => a - b);
  const mean = valid.reduce((acc, v) => acc + v, 0) / valid.length;
  const p05 = sorted[Math.max(0, Math.floor((sorted.length - 1) * 0.05))]!;
  const p95 = sorted[Math.min(sorted.length - 1, Math.ceil((sorted.length - 1) * 0.95))]!;
  const minVal = sorted[0]!;
  const maxVal = sorted[sorted.length - 1]!;
  const span = maxVal - minVal;
  if (span <= 1e-12) return null;
  const width = span / bucketCount;
  const bins: ReturnBin[] = Array.from({ length: bucketCount }, (_, i) => ({
    from: minVal + i * width,
    to: minVal + (i + 1) * width,
    count: 0,
    center: minVal + (i + 0.5) * width,
  }));
  for (const val of valid) {
    const idx = Math.min(bucketCount - 1, Math.max(0, Math.floor((val - minVal) / width)));
    bins[idx]!.count += 1;
  }
  const maxCount = Math.max(...bins.map(b => b.count), 1);
  return { bins, mean, p05, p95, minVal, maxVal, maxCount, total: valid.length };
}

export function metricDeltaText(
  current: number | undefined,
  previous: number | undefined,
  percent: boolean,
) {
  if (
    current === undefined ||
    previous === undefined ||
    !Number.isFinite(current) ||
    !Number.isFinite(previous)
  )
    return "";
  const delta = current - previous;
  if (Math.abs(delta) < (percent ? 5e-5 : 5e-4)) return "";
  const sign = delta > 0 ? "+" : "";
  return percent ? `${sign}${(delta * 100).toFixed(2)}%` : `${sign}${delta.toFixed(3)}`;
}

export type VerdictItem = { tone: "ok" | "attention"; label: string; detail: string };

export function evaluateRunVerdicts(
  summary: RunSummary,
  record: {
    data?: {
      rejections?: Array<{ count: number }>;
      diagnostics?: {
        costs?: {
          gross_pnl?: number;
          fees?: number;
          estimated_slippage?: number;
          net_pnl?: number;
        };
        by_symbol?: Array<{ symbol: string; net_pnl: number }>;
        trials?: { strategy_runs?: number; holdout_runs?: number; sharpe_std?: number | null };
        uses_pit_fundamentals?: boolean;
      };
    };
  },
  dayCount: number,
): VerdictItem[] {
  const items: VerdictItem[] = [];
  const metrics = summary.metrics;
  const diag = record.data?.diagnostics;
  const refused = (record.data?.rejections ?? []).reduce((acc, row) => acc + row.count, 0);

  const devSharpe = metrics.development_sharpe;
  const holdSharpe = metrics.holdout_sharpe;
  if (
    devSharpe !== undefined &&
    holdSharpe !== undefined &&
    devSharpe > 0.3 &&
    (holdSharpe <= 0 || holdSharpe < devSharpe * 0.5)
  ) {
    items.push({
      tone: "attention",
      label: t("留出衰减"),
      detail: t("留出前夏普 {dev} → 留出段 {hold}", {
        dev: devSharpe.toFixed(2),
        hold: holdSharpe.toFixed(2),
      }),
    });
  }

  const gross = diag?.costs?.gross_pnl;
  const costSum = (diag?.costs?.fees ?? 0) + (diag?.costs?.estimated_slippage ?? 0);
  if (gross !== undefined && gross > 0 && costSum / gross > 0.4) {
    items.push({
      tone: "attention",
      label: t("成本偏高"),
      detail: t("手续费与滑点占毛盈亏 {pct}%", { pct: ((costSum / gross) * 100).toFixed(0) }),
    });
  }

  const bySymbol = diag?.by_symbol ?? [];
  const net = diag?.costs?.net_pnl;
  if (bySymbol.length >= 2 && net !== undefined && net > 0) {
    const top = [...bySymbol].sort((a, b) => b.net_pnl - a.net_pnl)[0];
    if (top && top.net_pnl / net > 0.7) {
      items.push({
        tone: "attention",
        label: t("品种集中"),
        detail: t("{symbol} 贡献净盈亏 {pct}%", {
          symbol: top.symbol,
          pct: ((top.net_pnl / net) * 100).toFixed(0),
        }),
      });
    }
  }

  if (refused > 0) {
    items.push({
      tone: "attention",
      label: t("存在拒单"),
      detail: t("{count} 次目标被风控或资金拒绝", { count: refused }),
    });
  }

  const strategyTrials = diag?.trials?.strategy_runs ?? 0;
  const holdoutTrials = diag?.trials?.holdout_runs ?? 0;
  if (holdoutTrials >= 15 || strategyTrials >= 30) {
    const sharpeStd = diag?.trials?.sharpe_std;
    const dsr =
      metrics.sharpe !== undefined && dayCount >= 20 && sharpeStd != null && sharpeStd > 0
        ? (deflatedSharpeFromStd(
            metrics.sharpe,
            Math.max(strategyTrials, holdoutTrials),
            sharpeStd,
            dayCount,
          )?.dsr ?? null)
        : null;
    items.push({
      tone: "attention",
      label: t("高频试错"),
      detail:
        dsr !== null
          ? t("累计 {total} 次（同留出 {hold} 次）· 跨实验 DSR {dsr}%", {
              total: strategyTrials,
              hold: holdoutTrials,
              dsr: (dsr * 100).toFixed(0),
            })
          : t("累计测试 {total} 次（同留出 {hold} 次）", {
              total: strategyTrials,
              hold: holdoutTrials,
            }),
    });
  }

  if (diag?.uses_pit_fundamentals) {
    items.push({
      tone: "attention",
      label: t("PIT 基本面"),
      detail: t("Trading 实盘暂无实时基本面更新流"),
    });
  }

  if (!items.length && (metrics.fills ?? 0) > 0) {
    items.push({
      tone: "ok",
      label: t("体检通过"),
      detail: t("无留出衰减、成本过载、拒单或实盘流缺口警示"),
    });
  }
  return items;
}
