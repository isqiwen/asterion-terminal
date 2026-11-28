import { useEffect, useMemo, useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { lab } from "../../src/bridge/lab";
import type { RunDetail, RunReport } from "../../src/generated/asterion/lab_pb";
import { useErrorNotification } from "../../src/host/notifications";
import { getLocale, type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { warehouseName } from "../../src/ui/contract-names";
import { computeDrawdowns, returnHistogram, type DrawdownEpisode } from "./research-analytics";
import { type BenchmarkData, day, metric, signed } from "./result-parts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const sectorLabels: Record<string, string> = {
  black: "黑色",
  nonferrous: "有色",
  precious: "贵金属",
  energy_chem: "能化",
  oilseeds: "油脂油料",
  agriculture: "农副",
  financial: "金融",
  other: "其他",
};

const sessionLabels: Record<string, string> = {
  night: "夜盘",
  morning_1: "早盘一",
  morning_2: "早盘二",
  afternoon: "午盘",
  opening_15m: "开盘 15m",
  other: "其他",
};

export type DailyRiskSeries = {
  days: string[];
  margin_ratio: number[];
  gross_leverage: number[];
  net_exposure: number[];
  daily_return: number[];
};

export type DiagnosticsData = {
  trades?: {
    count?: number;
    wins?: number;
    losses?: number;
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
    traded_notional?: number;
    annual_turnover?: number | null;
  };
  exposure?: {
    avg_margin_ratio?: number | null;
    peak_margin_ratio?: number | null;
    avg_gross_leverage?: number | null;
    peak_gross_leverage?: number | null;
    avg_net_exposure?: number | null;
    max_drawdown_days?: number;
  };
  direction?: {
    long_pnl?: number;
    short_pnl?: number;
    long_trades?: number;
    short_trades?: number;
    long_win_rate?: number | null;
    short_win_rate?: number | null;
  };
  daily?: DailyRiskSeries;
  drawdowns?: DrawdownEpisode[];
  benchmark?: BenchmarkData | null;
  trials?: { strategy_runs?: number; holdout_runs?: number };
  significance?: {
    n_days?: number;
    trials?: number;
    skewness?: number | null;
    excess_kurtosis?: number | null;
    psr?: number | null;
    dsr?: number | null;
    expected_max_sharpe?: number | null;
    nw_t?: number | null;
    nw_p?: number | null;
    bootstrap?: {
      alpha?: number;
      block_size?: number;
      samples?: number;
      sharpe_ci?: [number, number] | null;
      annual_return_ci?: [number, number] | null;
      max_drawdown_ci?: [number, number] | null;
    } | null;
  };
  uses_pit_fundamentals?: boolean;
  by_symbol?: Array<{
    symbol: string;
    sector: string;
    gross_pnl: number;
    fees: number;
    slippage_cost: number;
    net_pnl: number;
    long_pnl: number;
    short_pnl: number;
    fills: number;
  }>;
  by_sector?: Array<{
    sector: string;
    gross_pnl: number;
    fees: number;
    slippage_cost: number;
    net_pnl: number;
    long_pnl: number;
    short_pnl: number;
    fills: number;
  }>;
  by_session?: Array<{
    session: string;
    trades: number;
    wins: number;
    win_rate?: number | null;
    gross_pnl: number;
    fees: number;
    slippage_cost: number;
    net_pnl: number;
    fills: number;
  }>;
  components?: Array<{
    name: string;
    share: number;
    ready_rate: number;
    active_bars: number;
    total_bars: number;
    avg_gross_weight: number;
    avg_contribution_weight: number;
    proxy_return: number;
  }>;
};

function money(value: number | null | undefined) {
  if (value === undefined || value === null || !Number.isFinite(value)) return "—";
  return new Intl.NumberFormat(getLocale(), {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  }).format(value);
}

function ReturnDistribution({ returns }: { returns: readonly number[] }) {
  const hist = useMemo(() => returnHistogram(returns), [returns]);
  if (!hist) return null;
  const zeroPct =
    hist.maxVal > hist.minVal
      ? Math.max(0, Math.min(100, ((0 - hist.minVal) / (hist.maxVal - hist.minVal)) * 100))
      : 50;
  return (
    <div className="lab-dist-card">
      <div className="lab-yearly-head">
        <h3>{t("日收益分布")}</h3>
        <span className="subtle numeric">
          {t("均值 {mean} · P05 {p05} · P95 {p95}", {
            mean: metric(hist.mean, true),
            p05: metric(hist.p05, true),
            p95: metric(hist.p95, true),
          })}
        </span>
      </div>
      <div className="lab-hist" role="img" aria-label={t("日收益分布")}>
        <span className="lab-hist-zero" style={{ left: `${zeroPct}%` }} aria-hidden="true" />
        {hist.bins.map((bin, idx) => (
          <div
            key={idx}
            className="lab-hist-col"
            title={`${metric(bin.from, true)} ~ ${metric(bin.to, true)}: ${bin.count}`}
          >
            <span
              className={`lab-hist-bar ${bin.center >= 0 ? "lab-up" : "lab-down"}`}
              style={{
                height: `${Math.max(bin.count > 0 ? 4 : 0, (bin.count / hist.maxCount) * 100)}%`,
              }}
            />
          </div>
        ))}
      </div>
      <div className="lab-hist-axis subtle numeric">
        <span>{metric(hist.minVal, true)}</span>
        <span>0%</span>
        <span>{metric(hist.maxVal, true)}</span>
      </div>
    </div>
  );
}

const quantStats = [
  ["sortino", "索提诺比率", false],
  ["omega", "Omega 比率", false],
  ["tail_ratio", "尾部比率", false],
  ["ulcer_index", "溃疡指数", true],
  ["value_at_risk", "日 VaR（95%）", true],
  ["conditional_value_at_risk", "日 CVaR（95%）", true],
] as const;

// QuantStats figures of the run, computed by the lab when first shown.
function QuantStats({
  id,
  annualPeriods,
  zeroRiskFree,
}: {
  id: string;
  annualPeriods?: number | null;
  zeroRiskFree?: boolean;
}) {
  const [report, setReport] = useState<RunReport | null>(null);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void lab({ case: "getRunReport", value: { id } })
      .then(reply => active && setReport(reply))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [id]);
  useErrorNotification(error, "asterion.terminal.research");
  return (
    <div>
      {error ? null : !report ? (
        <p className="subtle">{t("正在计算…")}</p>
      ) : (
        <>
          <dl className="lab-figures">
            {quantStats.map(([name, label, percent]) => (
              <div key={name}>
                <dt>{t(label)}</dt>
                <dd>{metric(report.figures[name], percent)}</dd>
              </div>
            ))}
          </dl>
          <p className="subtle">
            QuantStats {report.quantstatsVersion} ·{" "}
            {[
              t("日收益"),
              zeroRiskFree ? t("无风险利率 0") : "",
              annualPeriods ? t("一年 {count} 期", { count: annualPeriods }) : "",
            ]
              .filter(Boolean)
              .join(" · ")}
          </p>
        </>
      )}
    </div>
  );
}

export function RunDiagnostics({
  id,
  detail,
  record,
  focusedDay,
  onFocusChange,
}: {
  id: string;
  detail: RunDetail;
  record: {
    costs?: { deposit?: string };
    data?: { diagnostics?: DiagnosticsData };
    statistics?: { annual_periods?: number | null; zero_risk_free?: boolean };
  };
  focusedDay: string | null;
  onFocusChange: (time: string | null) => void;
}) {
  const deposit = Number(record.costs?.deposit ?? 0);
  const diag = useMemo<DiagnosticsData>(() => {
    if (record.data?.diagnostics) return record.data.diagnostics;
    // Fallback for runs recorded before diagnostics were persisted.
    let peak = Number.isFinite(deposit) && deposit > 0 ? deposit : 0;
    let curDd = 0;
    let maxDd = 0;
    let marginSum = 0;
    let marginPeak = 0;
    let marginDays = 0;
    let fees = 0;
    for (const item of detail.days) {
      const eq = Number(item.equity?.text ?? 0);
      const margin = Number(item.margin?.text ?? 0);
      fees += Number(item.fees?.text ?? 0);
      if (eq >= peak) {
        peak = eq;
        curDd = 0;
      } else {
        curDd += 1;
        if (curDd > maxDd) maxDd = curDd;
      }
      if (eq > 0) {
        const ratio = margin / eq;
        marginSum += ratio;
        marginPeak = Math.max(marginPeak, ratio);
        marginDays += 1;
      }
    }
    const lastEq = detail.days.length
      ? Number(detail.days.at(-1)!.equity?.text ?? deposit)
      : deposit;
    const netPnl = lastEq - deposit;
    return {
      costs: { gross_pnl: netPnl + fees, fees, net_pnl: netPnl },
      exposure: {
        avg_margin_ratio: marginDays ? marginSum / marginDays : null,
        peak_margin_ratio: marginDays ? marginPeak : null,
        max_drawdown_days: maxDd,
      },
    };
  }, [detail.days, deposit, record]);

  const trades = diag.trades ?? {};
  const costs = diag.costs ?? {};
  const exposure = diag.exposure ?? {};
  const direction = diag.direction ?? {};
  const significance = diag.significance;
  const bySymbol = diag.by_symbol ?? [];
  const bySector = diag.by_sector ?? [];
  const bySession = diag.by_session ?? [];
  const components = diag.components ?? [];
  const drawdowns = useMemo(
    () => (diag.drawdowns?.length ? diag.drawdowns : computeDrawdowns(detail.days, deposit)),
    [diag.drawdowns, detail.days, deposit],
  );
  const dailyReturns = useMemo(() => {
    if (diag.daily?.daily_return?.length) return diag.daily.daily_return;
    let prev = deposit;
    return detail.days.map(item => {
      const eq = Number(item.equity?.text ?? 0);
      const r = prev > 0 ? eq / prev - 1 : 0;
      prev = eq;
      return r;
    });
  }, [diag.daily, detail.days, deposit]);

  const riskChart = useMemo(() => {
    if (!detail.days.length) return null;
    const timeAt = (index: number) => String(detail.days[index].settledMs * 1_000_000n);
    const hasLeverage = Boolean(diag.daily?.days?.length && diag.daily.gross_leverage.length);
    if (hasLeverage) {
      const idxByDay = new Map(diag.daily!.days.map((d, i) => [d, i]));
      const gross = detail.days.map(item => {
        const i = idxByDay.get(item.tradingDay);
        return i === undefined ? null : diag.daily!.gross_leverage[i]!;
      });
      const net = detail.days.map(item => {
        const i = idxByDay.get(item.tradingDay);
        return i === undefined ? null : diag.daily!.net_exposure[i]!;
      });
      const margin = detail.days.map(item => {
        const i = idxByDay.get(item.tradingDay);
        return i === undefined ? null : diag.daily!.margin_ratio[i]!;
      });
      return {
        label: t("过程风险敞口（杠杆 / 净敞口 / 保证金率）"),
        series: {
          kind: "line" as const,
          points: detail.days.map((_, idx) => ({
            timestamp_ns: timeAt(idx),
            price: gross[idx] === null ? "" : gross[idx]!.toFixed(3),
          })),
        },
        overlays: [
          { label: t("净敞口"), color: "var(--series-1)", values: net },
          { label: t("保证金率"), color: "var(--series-3)", values: margin },
        ],
        describe: (index: number) =>
          `${detail.days[index].tradingDay} · ${t("总杠杆")} ${gross[index]?.toFixed(2) ?? "—"}x · ${t("净敞口")} ${metric(net[index] ?? undefined, true)} · ${t("保证金率")} ${metric(margin[index] ?? undefined, true)}`,
      };
    }
    const margin = detail.days.map(item => {
      const eq = Number(item.equity?.text ?? 0);
      const m = Number(item.margin?.text ?? 0);
      return eq > 0 ? m / eq : 0;
    });
    return {
      label: t("每日保证金占用率（%）"),
      series: {
        kind: "line" as const,
        points: detail.days.map((_, idx) => ({
          timestamp_ns: timeAt(idx),
          price: (margin[idx]! * 100).toFixed(2),
        })),
      },
      overlays: undefined,
      describe: (index: number) =>
        `${detail.days[index].tradingDay} · ${t("保证金率")} ${metric(margin[index], true)}`,
    };
  }, [detail.days, diag.daily]);

  const maxCostAbs = Math.max(
    Math.abs(costs.gross_pnl ?? 0),
    Math.abs(costs.fees ?? 0),
    Math.abs(costs.estimated_slippage ?? 0),
    Math.abs(costs.net_pnl ?? 0),
    1e-9,
  );
  const maxDirAbs = Math.max(
    Math.abs(direction.long_pnl ?? 0),
    Math.abs(direction.short_pnl ?? 0),
    1e-9,
  );
  const maxSymAbs = Math.max(...bySymbol.map(row => Math.abs(row.net_pnl)), 1e-9);
  const maxSecAbs = Math.max(...bySector.map(row => Math.abs(row.net_pnl)), 1e-9);
  const maxSessAbs = Math.max(...bySession.map(row => Math.abs(row.net_pnl)), 1e-9);

  const sectorName = (code: string) => t(sectorLabels[code] ?? "其他");
  const sessionName = (code: string) => t(sessionLabels[code] ?? "其他");
  const symbolLabel = (symbol: string) => {
    const name = warehouseName(symbol);
    return name ? `${name} ${symbol}` : symbol;
  };
  return (
    <>
      <h3>{t("交易质量与风控摘要")}</h3>
      <dl className="lab-kv-strip">
        <div>
          <dt>{t("回合数")}</dt>
          <dd>{trades.count ?? "—"}</dd>
        </div>
        <div>
          <dt>{t("胜率")}</dt>
          <dd>{metric(trades.win_rate ?? undefined, true)}</dd>
        </div>
        <div>
          <dt>{t("盈亏比")}</dt>
          <dd>{metric(trades.payoff_ratio ?? undefined, false)}</dd>
        </div>
        <div>
          <dt>{t("盈利因子")}</dt>
          <dd>{metric(trades.profit_factor ?? undefined, false)}</dd>
        </div>
        <div>
          <dt>{t("平均持仓")}</dt>
          <dd>
            {trades.avg_holding_days != null
              ? t("{days} 天", { days: trades.avg_holding_days.toFixed(1) })
              : "—"}
          </dd>
        </div>
        <div>
          <dt>{t("年化换手")}</dt>
          <dd>{costs.annual_turnover != null ? `${costs.annual_turnover.toFixed(2)}x` : "—"}</dd>
        </div>
        <div>
          <dt>{t("最长水下")}</dt>
          <dd>
            {exposure.max_drawdown_days !== undefined
              ? t("{days} 天", { days: exposure.max_drawdown_days })
              : "—"}
          </dd>
        </div>
        <div>
          <dt>{t("峰值保证金率")}</dt>
          <dd>{metric(exposure.peak_margin_ratio ?? undefined, true)}</dd>
        </div>
        <div>
          <dt>{t("峰值杠杆")}</dt>
          <dd>
            {exposure.peak_gross_leverage != null
              ? `${exposure.peak_gross_leverage.toFixed(2)}x`
              : "—"}
          </dd>
        </div>
        <div>
          <dt>{t("均值净敞口")}</dt>
          <dd className={signed(exposure.avg_net_exposure)}>
            {metric(exposure.avg_net_exposure ?? undefined, true)}
          </dd>
        </div>
      </dl>

      {significance && (
        <>
          <h3>{t("统计显著性与稳健性")}</h3>
          <dl className="lab-kv-strip" aria-label={t("统计显著性与稳健性")}>
            {significance.psr != null && (
              <div>
                <dt>{t("PSR 置信度")}</dt>
                <dd>{metric(significance.psr, true)}</dd>
              </div>
            )}
            {significance.dsr != null && (
              <div>
                <dt>{t("DSR 置信度")}</dt>
                <dd>{metric(significance.dsr, true)}</dd>
              </div>
            )}
            {significance.nw_t != null && (
              <div>
                <dt>{t("Newey-West t")}</dt>
                <dd>
                  {metric(significance.nw_t, false)}
                  {significance.nw_p != null ? ` (p=${significance.nw_p.toFixed(3)})` : ""}
                </dd>
              </div>
            )}
            {significance.skewness != null && significance.excess_kurtosis != null && (
              <div>
                <dt>{t("偏度 / 超额峰度")}</dt>
                <dd>
                  {metric(significance.skewness, false)} /{" "}
                  {metric(significance.excess_kurtosis, false)}
                </dd>
              </div>
            )}
            {significance.bootstrap?.sharpe_ci && (
              <div>
                <dt>{t("夏普 95% 区间")}</dt>
                <dd>
                  [{metric(significance.bootstrap.sharpe_ci[0], false)},{" "}
                  {metric(significance.bootstrap.sharpe_ci[1], false)}]
                </dd>
              </div>
            )}
            {significance.bootstrap?.annual_return_ci && (
              <div>
                <dt>{t("年化收益 95% 区间")}</dt>
                <dd>
                  [{metric(significance.bootstrap.annual_return_ci[0], true)},{" "}
                  {metric(significance.bootstrap.annual_return_ci[1], true)}]
                </dd>
              </div>
            )}
            {significance.bootstrap?.max_drawdown_ci && (
              <div>
                <dt>{t("最大回撤 95% 区间")}</dt>
                <dd>
                  [{metric(significance.bootstrap.max_drawdown_ci[0], true)},{" "}
                  {metric(significance.bootstrap.max_drawdown_ci[1], true)}]
                </dd>
              </div>
            )}
          </dl>
        </>
      )}

      <div className="lab-segments">
        <div className="lab-flow-card">
          <h3>{t("成本拆解")}</h3>
          <dl className="lab-flow-list">
            {(
              [
                [t("毛盈亏"), costs.gross_pnl, signed(costs.gross_pnl)],
                [t("手续费"), costs.fees ? -Math.abs(costs.fees) : costs.fees, "subtle"],
                [
                  t("估算滑点"),
                  costs.estimated_slippage
                    ? -Math.abs(costs.estimated_slippage)
                    : costs.estimated_slippage,
                  "subtle",
                ],
                [t("净盈亏"), costs.net_pnl, signed(costs.net_pnl)],
              ] as const
            ).map(([label, val, tone]) => (
              <div key={label} className="lab-flow-row">
                <dt>{label}</dt>
                <dd className="lab-flow-track" aria-hidden="true">
                  <span
                    className={`lab-bar ${tone === "subtle" ? "lab-cost-bar" : tone}`}
                    style={{ width: `${(100 * Math.abs(val ?? 0)) / maxCostAbs}%` }}
                  />
                </dd>
                <dd className={`numeric ${tone === "subtle" ? "" : tone}`}>{money(val)}</dd>
              </div>
            ))}
          </dl>
        </div>
        <div className="lab-flow-card">
          <h3>{t("多空方向")}</h3>
          <dl className="lab-flow-list">
            {(
              [
                [t("多头盈亏"), direction.long_pnl, direction.long_trades, direction.long_win_rate],
                [
                  t("空头盈亏"),
                  direction.short_pnl,
                  direction.short_trades,
                  direction.short_win_rate,
                ],
              ] as const
            ).map(([label, val, count, wr]) => (
              <div key={label} className="lab-flow-row">
                <dt>
                  {label}
                  {count !== undefined && (
                    <small className="subtle">
                      {" "}
                      {count} · {metric(wr ?? undefined, true)}
                    </small>
                  )}
                </dt>
                <dd className="lab-flow-track" aria-hidden="true">
                  <span
                    className={`lab-bar ${signed(val)}`}
                    style={{ width: `${(100 * Math.abs(val ?? 0)) / maxDirAbs}%` }}
                  />
                </dd>
                <dd className={`numeric ${signed(val)}`}>{money(val)}</dd>
              </div>
            ))}
          </dl>
        </div>
      </div>

      {riskChart && (
        <PriceChart
          series={riskChart.series}
          overlays={riskChart.overlays}
          label={riskChart.label}
          titled
          compact
          volumeLabel={t("成交量")}
          showVolume={false}
          baselineValue={0}
          focusedTime={focusedDay}
          onFocusChange={onFocusChange}
          formatTime={value => day(BigInt(value))}
          describe={riskChart.describe}
        />
      )}

      {(dailyReturns.length >= 3 || drawdowns.length > 0) && (
        <div className="lab-segments">
          <ReturnDistribution returns={dailyReturns} />
          {drawdowns.length > 0 && (
            <div>
              <h3>{t("最大回撤区间（Top 5）")}</h3>
              <div className="lab-scroll">
                <table className="lab-table" aria-label={t("最大回撤区间")}>
                  <thead>
                    <tr>
                      <th scope="col">{t("峰值日")}</th>
                      <th scope="col">{t("谷底日")}</th>
                      <th scope="col">{t("恢复日")}</th>
                      <th scope="col" className="numeric">
                        {t("回撤")}
                      </th>
                      <th scope="col" className="numeric">
                        {t("持续")}
                      </th>
                    </tr>
                  </thead>
                  <tbody>
                    {drawdowns.map(item => (
                      <tr key={`${item.peak_day}-${item.trough_day}`}>
                        <td className="numeric">{item.peak_day}</td>
                        <td className="numeric">{item.trough_day}</td>
                        <td className="numeric subtle">{item.recovery_day ?? t("未恢复")}</td>
                        <td className="numeric lab-down">{metric(item.depth, true)}</td>
                        <td className="numeric">{t("{days} 天", { days: item.duration_days })}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>
          )}
        </div>
      )}

      {bySymbol.length > 0 && (
        <>
          <h3>{t("分品种归因")}</h3>
          <div className="lab-scroll">
            <table className="lab-table" aria-label={t("分品种归因")}>
              <thead>
                <tr>
                  <th scope="col">{t("品种")}</th>
                  <th scope="col">{t("板块")}</th>
                  <th scope="col" className="numeric">
                    {t("净盈亏")}
                  </th>
                  <td aria-hidden="true" />
                  <th scope="col" className="numeric">
                    {t("毛盈亏")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("手续费")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("估算滑点")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("多头")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("空头")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("成交笔数")}
                  </th>
                </tr>
              </thead>
              <tbody>
                {bySymbol.map(row => (
                  <tr key={row.symbol}>
                    <th scope="row">{symbolLabel(row.symbol)}</th>
                    <td>{sectorName(row.sector)}</td>
                    <td className={`numeric ${signed(row.net_pnl)}`}>{money(row.net_pnl)}</td>
                    <td className="lab-bar-cell" aria-hidden="true">
                      <span
                        className={`lab-bar ${signed(row.net_pnl)}`}
                        style={{ width: `${(100 * Math.abs(row.net_pnl)) / maxSymAbs}%` }}
                      />
                    </td>
                    <td className={`numeric ${signed(row.gross_pnl)}`}>{money(row.gross_pnl)}</td>
                    <td className="numeric">{money(row.fees)}</td>
                    <td className="numeric">{money(row.slippage_cost)}</td>
                    <td className={`numeric ${signed(row.long_pnl)}`}>{money(row.long_pnl)}</td>
                    <td className={`numeric ${signed(row.short_pnl)}`}>{money(row.short_pnl)}</td>
                    <td className="numeric">{row.fills}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}

      {bySector.length > 1 && (
        <>
          <h3>{t("分板块归因")}</h3>
          <div className="lab-scroll">
            <table className="lab-table" aria-label={t("分板块归因")}>
              <thead>
                <tr>
                  <th scope="col">{t("板块")}</th>
                  <th scope="col" className="numeric">
                    {t("净盈亏")}
                  </th>
                  <td aria-hidden="true" />
                  <th scope="col" className="numeric">
                    {t("毛盈亏")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("手续费")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("多头")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("空头")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("成交笔数")}
                  </th>
                </tr>
              </thead>
              <tbody>
                {bySector.map(row => (
                  <tr key={row.sector}>
                    <th scope="row">{sectorName(row.sector)}</th>
                    <td className={`numeric ${signed(row.net_pnl)}`}>{money(row.net_pnl)}</td>
                    <td className="lab-bar-cell" aria-hidden="true">
                      <span
                        className={`lab-bar ${signed(row.net_pnl)}`}
                        style={{ width: `${(100 * Math.abs(row.net_pnl)) / maxSecAbs}%` }}
                      />
                    </td>
                    <td className={`numeric ${signed(row.gross_pnl)}`}>{money(row.gross_pnl)}</td>
                    <td className="numeric">{money(row.fees)}</td>
                    <td className={`numeric ${signed(row.long_pnl)}`}>{money(row.long_pnl)}</td>
                    <td className={`numeric ${signed(row.short_pnl)}`}>{money(row.short_pnl)}</td>
                    <td className="numeric">{row.fills}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}

      {bySession.length > 0 && (
        <>
          <h3>{t("日内分时段归因")}</h3>
          <div className="lab-scroll">
            <table className="lab-table" aria-label={t("日内分时段归因")}>
              <thead>
                <tr>
                  <th scope="col">{t("时段")}</th>
                  <th scope="col" className="numeric">
                    {t("净盈亏")}
                  </th>
                  <td aria-hidden="true" />
                  <th scope="col" className="numeric">
                    {t("毛盈亏")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("手续费")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("回合数")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("胜率")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("成交笔数")}
                  </th>
                </tr>
              </thead>
              <tbody>
                {bySession.map(row => (
                  <tr key={row.session}>
                    <th scope="row">{sessionName(row.session)}</th>
                    <td className={`numeric ${signed(row.net_pnl)}`}>{money(row.net_pnl)}</td>
                    <td className="lab-bar-cell" aria-hidden="true">
                      <span
                        className={`lab-bar ${signed(row.net_pnl)}`}
                        style={{ width: `${(100 * Math.abs(row.net_pnl)) / maxSessAbs}%` }}
                      />
                    </td>
                    <td className={`numeric ${signed(row.gross_pnl)}`}>{money(row.gross_pnl)}</td>
                    <td className="numeric">{money(row.fees)}</td>
                    <td className="numeric">{row.trades}</td>
                    <td className="numeric">{metric(row.win_rate ?? undefined, true)}</td>
                    <td className="numeric">{row.fills}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}

      {components.length > 0 && (
        <>
          <h3>{t("子策略过程与贡献")}</h3>
          <div className="lab-scroll">
            <table className="lab-table" aria-label={t("子策略过程与贡献")}>
              <thead>
                <tr>
                  <th scope="col">{t("子策略")}</th>
                  <th scope="col" className="numeric">
                    {t("配权")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("就绪率")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("平均毛权重")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("组合权重贡献")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("代理收益贡献")}
                  </th>
                </tr>
              </thead>
              <tbody>
                {components.map(row => (
                  <tr key={row.name}>
                    <th scope="row">{row.name}</th>
                    <td className="numeric">{metric(row.share, true)}</td>
                    <td className="numeric">{metric(row.ready_rate, true)}</td>
                    <td className="numeric">{metric(row.avg_gross_weight, true)}</td>
                    <td className="numeric">{metric(row.avg_contribution_weight, true)}</td>
                    <td className={`numeric ${signed(row.proxy_return)}`}>
                      {metric(row.proxy_return, true)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}

      <h3>{t("风险与尾部（QuantStats）")}</h3>
      {detail.days.length < 20 ? (
        <p className="subtle">
          {t("结算日不足 20 天（当前 {count} 天）", { count: detail.days.length })}
        </p>
      ) : (
        <QuantStats
          id={id}
          annualPeriods={record.statistics?.annual_periods}
          zeroRiskFree={record.statistics?.zero_risk_free}
        />
      )}
    </>
  );
}
