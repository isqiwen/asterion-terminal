import { Fragment, useEffect, useMemo, useState } from "react";
import {
  PriceChart,
  type Candle,
  type TradeMark,
  type VerticalMarker,
} from "@asterion/terminal/charts/PriceChart";
import { groupedText } from "../../src/bridge/decimal";
import { lab } from "../../src/bridge/lab";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import type { ChartBars, RunDetail, RunSummary } from "../../src/generated/asterion/lab_pb";
import { useErrorNotification } from "../../src/host/notifications";
import { getLocale, listSeparator, type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError, diagnosticSummary } from "../../src/i18n/errors";
import { protocolPeriod } from "../../src/ui/periods";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { dataGapParams, isMinutePeriod, parseDataGap } from "./data-gap";
import { CopySummaryButton, ExperimentNoteBar, useAnnotations } from "./LabParts";
import { evaluateRunVerdicts, metricDeltaText, monthlyReturns } from "./research-analytics";
import { runName, runState } from "./research-lab";
import { formatRunSummaryMarkdown } from "./research-markdown";
import {
  type BenchmarkData,
  day,
  figures,
  metric,
  minute,
  signed,
  states,
  VirtualScrollTable,
} from "./result-parts";
import { RunDiagnostics } from "./RunDiagnostics";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const offsets: Partial<Record<Offset, string>> = {
  [Offset.OPEN]: "开仓",
  [Offset.CLOSE_TODAY]: "平今",
  [Offset.CLOSE_YESTERDAY]: "平昨",
  [Offset.CLOSE]: "平仓",
};

// A target the run's limits or its funds refused, how often and when.
type Rejection = {
  contract: string;
  lots: string;
  reason: string;
  count: number;
  first_ms: number;
  last_ms: number;
};

const reasons: Record<string, string> = {
  order_quantity: "超过单笔上限",
  gross_quantity: "超过总持仓上限",
  working_orders: "超过在途委托上限",
  insufficient_funds: "资金不足",
};

function Figures({
  summary,
  prefix = "",
  previous,
}: {
  summary: RunSummary;
  prefix?: string;
  previous?: RunSummary;
}) {
  return (
    <dl className="lab-figures">
      {figures.map(([name, label, percent]) => {
        const value = summary.metrics[prefix + name];
        const delta = previous
          ? metricDeltaText(value, previous.metrics[prefix + name], percent)
          : "";
        return (
          <div key={name}>
            <dt>{t(label)}</dt>
            <dd className={name.endsWith("return") ? signed(value) : undefined}>
              {metric(value, percent)}
            </dd>
            {delta && (
              <dd
                className="lab-delta"
                title={t("相对上次运行 {name}", { name: runName(previous!) })}
              >
                Δ {delta}
              </dd>
            )}
          </div>
        );
      })}
    </dl>
  );
}

function SegmentFigures({
  summary,
  segments,
}: {
  summary: RunSummary;
  segments: readonly (readonly [string, string])[];
}) {
  return (
    <div className="lab-scroll">
      <table className="lab-table" aria-label={t("分段对比")}>
        <thead>
          <tr>
            <th scope="col">{t("分段")}</th>
            {figures.map(([name, label]) => (
              <th scope="col" key={name} className="numeric">
                {t(label)}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {segments.map(([label, prefix]) => (
            <tr key={prefix}>
              <th scope="row">{label}</th>
              {figures.map(([name, , percent]) => {
                const value = summary.metrics[prefix + name];
                return (
                  <td
                    key={name}
                    className={`numeric ${name.endsWith("return") ? signed(value) : ""}`}
                  >
                    {metric(value, percent)}
                  </td>
                );
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function RunChart({
  detail,
  data,
  contract,
  choose,
}: {
  detail: RunDetail;
  data: { bars: { period: string }; dataset_version?: string; begin_day: string; end_day: string };
  contract: string;
  choose: (value: string) => void;
}) {
  const [bars, setBars] = useState<ChartBars["bars"]>([]);
  const [notice, setNotice] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [retry, setRetry] = useState(0);
  useErrorNotification(error, "asterion.terminal.research");
  useEffect(() => {
    let active = true;
    setBars([]);
    setNotice("");
    setError("");
    if (!contract) return;
    // The bars the run read: days, or minutes of its period.
    const barMeta = data.bars;
    const minutes = barMeta.period === "day" ? 0 : Number(String(barMeta.period).replace("m", ""));
    const datasetVersion = data.dataset_version;
    if (!datasetVersion) {
      setNotice(t("运行记录缺少数据版本，无法读取对应 K 线。"));
      return;
    }
    void lab({
      case: "getChartBars",
      value: { contract, period: protocolPeriod(minutes || "day"), datasetVersion },
    })
      .then(reply => {
        if (!active) return;
        if (reply.datasetVersion !== datasetVersion) {
          setNotice(t("返回的 K 线与运行使用的数据版本不一致。"));
          return;
        }
        // Only the run's range of a contract's stored bars.
        const first = data.begin_day,
          last = data.end_day;
        setBars(reply.bars.filter(bar => bar.tradingDay >= first && bar.tradingDay <= last));
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [contract, data, retry]);
  const contracts = useMemo(
    () => [...new Set(detail.fills.map(fill => fill.contract))],
    [detail.fills],
  );
  const daily = data.bars.period === "day";
  const { candles, chartSeries, validTrades } = useMemo(() => {
    const nextCandles: Candle[] = bars.map(bar => ({
      timestamp_ns: String(bar.endMs * 1_000_000n),
      open: bar.open?.text ?? "",
      high: bar.high?.text ?? "",
      low: bar.low?.text ?? "",
      close: bar.close?.text ?? "",
      volume: bar.volume?.text ?? "0",
    }));
    const byDay = new Map<string, number[]>();
    bars.forEach((bar, index) => {
      const list = byDay.get(bar.tradingDay);
      if (list) list.push(index);
      else byDay.set(bar.tradingDay, [index]);
    });
    const nextTrades: TradeMark[] = detail.fills
      .filter(fill => fill.contract === contract)
      .map(fill => {
        // A day's fills sit on its bar; a minute fill on the bar that covers its execution time.
        const indices = byDay.get(fill.tradingDay) ?? [];
        const index = daily
          ? (indices[0] ?? -1)
          : (indices.find(item => bars[item].endMs >= fill.timeMs) ?? -1);
        return {
          index,
          side: fill.side === Side.BUY ? ("buy" as const) : ("sell" as const),
          text: `${fill.side === Side.BUY ? t("买") : t("卖")} ${Number(fill.lots?.text)}@${Number(fill.price?.text)}`,
        };
      })
      .filter(trade => trade.index >= 0);
    return {
      candles: nextCandles,
      chartSeries: { kind: "candles" as const, points: nextCandles },
      validTrades: nextTrades,
    };
  }, [bars, detail.fills, contract, daily]);
  return contracts.length ? (
    <>
      <label className="lab-inline">
        {t("合约")}
        <select value={contract} onChange={event => choose(event.target.value)}>
          {contracts.map(item => (
            <option key={item}>{item}</option>
          ))}
        </select>
      </label>
      {notice && <p role="status">{notice}</p>}
      {error && (
        <button type="button" onClick={() => setRetry(value => value + 1)}>
          {t("重试")}
        </button>
      )}
      {candles.length > 0 && (
        <PriceChart
          series={chartSeries}
          label={contract}
          volumeLabel={t("成交量")}
          trades={validTrades}
          formatTime={value => (daily ? day(BigInt(value)) : minute(BigInt(value)))}
          describe={index =>
            `${daily ? bars[index].tradingDay : minute(bars[index].endMs * 1_000_000n)} · ${bars[index].open?.text} ${bars[index].high?.text} ${bars[index].low?.text} ${bars[index].close?.text}`
          }
        />
      )}
    </>
  ) : (
    <p className="subtle">{t("没有成交")}</p>
  );
}

function monthlyTint(value: number | undefined, scale: number) {
  if (value === undefined || !Number.isFinite(value) || value === 0 || scale <= 0) return undefined;
  const ratio = Math.min(1, Math.abs(value) / scale);
  const strength = Math.round(4 + ratio * 7);
  const tone = value > 0 ? "var(--quote-up)" : "var(--quote-down)";
  return { "--cell-tint": `color-mix(in oklab, ${tone} ${strength}%, var(--canvas))` } as Record<
    string,
    string
  >;
}

// Each month's return, a year to a row, with the year's compounded total.
function MonthlyReturns({ months }: { months: { month: string; value: number }[] }) {
  const years = [...new Set(months.map(item => item.month.slice(0, 4)))];
  const of = new Map(months.map(item => [item.month, item.value]));
  const monthScale = Math.max(0.02, ...months.map(item => Math.abs(item.value)));
  const monthName = new Intl.DateTimeFormat(getLocale(), { month: "short", timeZone: "UTC" });
  const headers = Array.from({ length: 12 }, (_, index) =>
    monthName.format(new Date(Date.UTC(2026, index, 1))),
  );
  return (
    <div className="lab-scroll" tabIndex={0} role="region" aria-label={t("月度收益")}>
      <table className="lab-table lab-monthly" aria-label={t("月度收益")}>
        <colgroup>
          <col className="lab-monthly-year" />
          {headers.map((_, index) => (
            <col key={index} />
          ))}
          <col className="lab-monthly-total" />
        </colgroup>
        <thead>
          <tr>
            <th scope="col">
              <span className="sr-only">{t("年份")}</span>
            </th>
            {headers.map((label, index) => (
              <th scope="col" key={index} className="numeric">
                {label}
              </th>
            ))}
            <th scope="col" className="numeric">
              {t("全年")}
            </th>
          </tr>
        </thead>
        <tbody>
          {years.map(year => {
            const values = Array.from({ length: 12 }, (_, index) =>
              of.get(`${year}-${String(index + 1).padStart(2, "0")}`),
            );
            const total =
              values.reduce<number>((product, value) => product * (1 + (value ?? 0)), 1) - 1;
            const totalScale = Math.max(0.06, monthScale * 2.5, Math.abs(total));
            return (
              <tr key={year}>
                <th scope="row">{year}</th>
                {values.map((value, index) => (
                  <td
                    key={index}
                    className={`numeric ${signed(value)}`}
                    style={monthlyTint(value, monthScale)}
                  >
                    {value === undefined ? (
                      <span className="lab-monthly-empty" aria-hidden="true">
                        —
                      </span>
                    ) : (
                      metric(value, true)
                    )}
                  </td>
                ))}
                <td className={`numeric ${signed(total)}`} style={monthlyTint(total, totalScale)}>
                  {metric(total, true)}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

// What reproduces the run: strategy version, data, engine, costs, lineage and limits.
function RunDetails({
  record,
  openVersion,
  openFactor,
  openRun,
}: {
  record: string;
  openVersion?: (version: string) => void;
  openFactor?: (id: string) => void;
  openRun?: (id: string) => void;
}) {
  const value = JSON.parse(record);
  const lineage = value.data?.lineage as
    { factor_evaluation?: string; source_runs?: string[] } | undefined;
  const trials = value.data?.diagnostics?.trials as
    { strategy_runs?: number; holdout_runs?: number } | undefined;
  return (
    <div className="lab-details">
      <dl>
        <dt>{t("策略")}</dt>
        <dd>
          {value.strategy.id}
          {value.strategy.commit && (
            <>
              {" @ "}
              <code>{value.strategy.commit.slice(0, 12)}</code>
            </>
          )}
          {value.strategy.changes
            ? ` · ${t("含未提交改动")} ${value.strategy.changes.slice(0, 12)}`
            : ""}
        </dd>
        <dt>{t("数据")}</dt>
        <dd>
          {t("研究数据")} ·{" "}
          {openVersion ? (
            <button
              type="button"
              className="lab-link"
              onClick={() => openVersion(value.data.dataset_version)}
            >
              <code>{value.data.dataset_version.slice(0, 12)}</code>
            </button>
          ) : (
            <code>{value.data.dataset_version.slice(0, 12)}</code>
          )}{" "}
          · {value.data.symbols.join(listSeparator())} · {value.data.begin_day} →{" "}
          {value.data.end_day}
        </dd>
        <dt>{t("引擎")}</dt>
        <dd>{value.engine.version}</dd>
        {trials?.strategy_runs ? (
          <>
            <dt>{t("累计尝试")}</dt>
            <dd>
              {t("该策略累计 {total} 次成功运行", { total: trials.strategy_runs })}
              {trials.holdout_runs
                ? ` · ${t("同留出期 {hold} 次", { hold: trials.holdout_runs })}`
                : ""}
            </dd>
          </>
        ) : null}
        {lineage?.factor_evaluation && (
          <>
            <dt>{t("来源因子评价")}</dt>
            <dd>
              {openFactor ? (
                <button
                  type="button"
                  className="lab-link"
                  onClick={() => openFactor(lineage.factor_evaluation!)}
                >
                  <code>{lineage.factor_evaluation}</code>
                </button>
              ) : (
                <code>{lineage.factor_evaluation}</code>
              )}
            </dd>
          </>
        )}
        {lineage?.source_runs && lineage.source_runs.length > 0 && (
          <>
            <dt>{t("来源子策略回测")}</dt>
            <dd className="lab-lineage-links">
              {lineage.source_runs.map(runId =>
                openRun ? (
                  <button
                    key={runId}
                    type="button"
                    className="lab-link"
                    onClick={() => openRun(runId)}
                  >
                    <code>{runId}</code>
                  </button>
                ) : (
                  <code key={runId}>{runId}</code>
                ),
              )}
            </dd>
          </>
        )}
      </dl>
      <pre>
        {JSON.stringify({ params: value.params, costs: value.costs, risk: value.risk }, null, 2)}
      </pre>
    </div>
  );
}

// One run's result: its figures, equity, trades on the bars, months and what
// reproduces it. Read again when `state`, the run's state in the list of
// runs, changes, so a running run shows its result once it has ended.
// `openVersion` shows a research dataset version on the data page.
export function RunResult({
  id,
  state,
  previous,
  openVersion,
  openDataset,
  openFactor,
  openRun,
}: {
  id: string;
  state: string;
  previous?: RunSummary;
  openVersion?: (version: string) => void;
  openDataset?: (params: Record<string, string>) => void;
  openFactor?: (id: string) => void;
  openRun?: (id: string) => void;
}) {
  const [detail, setDetail] = useState<RunDetail | null>(null);
  const [contract, setContract] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [tab, setTab] = useState<"overview" | "diagnostics" | "bars" | "fills" | "days" | "record">(
    "overview",
  );
  const [focusedDay, setFocusedDay] = useState<string | null>(null);
  const [showBenchmark, setShowBenchmark] = useState(true);
  const annotations = useAnnotations();
  useEffect(() => {
    let active = true;
    setDetail(null);
    setError("");
    setFocusedDay(null);
    void lab({ case: "getRun", value: { id } })
      .then(reply => {
        if (!active) return;
        setDetail(reply);
        setContract(reply.fills[0]?.contract ?? "");
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [id, state]);
  const record = useMemo(() => (detail ? JSON.parse(detail.record) : null), [detail]);
  const overview = useMemo(() => {
    if (!detail?.summary || !record || runState(detail.summary.state) !== "succeeded") return null;
    const holdout = record.statistics?.holdout_day as string | undefined;
    const rolling = record.statistics?.rolling;
    const splitMarkers: VerticalMarker[] = [];
    if (holdout) {
      const splitIndex = detail.days.findIndex(item => item.tradingDay >= holdout);
      if (splitIndex > 0)
        splitMarkers.push({
          index: splitIndex,
          label: `${t("留出段")} ${detail.days[splitIndex].tradingDay}`,
        });
    } else if (rolling) {
      const firstOosDay =
        (rolling.rounds?.[0]?.first_day as string | undefined) ??
        detail.days[Number(rolling.training_days)]?.tradingDay;
      const splitIndex = firstOosDay
        ? detail.days.findIndex(item => item.tradingDay >= firstOosDay)
        : -1;
      if (splitIndex > 0)
        splitMarkers.push({
          index: splitIndex,
          label: `${t("样本外")} ${detail.days[splitIndex].tradingDay}`,
        });
    }
    const equity = detail.days.map(item => Number(item.equity?.text));
    const deposit = Number(record.costs?.deposit);
    let peak = Number.isFinite(deposit) && deposit > 0 ? deposit : -Infinity;
    const drawdown = equity.map(value => {
      peak = Math.max(peak, value);
      return peak > 0 ? value / peak - 1 : 0;
    });
    const timeAt = (index: number) => String(detail.days[index].settledMs * 1_000_000n);
    return {
      holdout,
      rolling,
      splitMarkers,
      deposit,
      drawdown,
      equitySeries: {
        kind: "line" as const,
        points: detail.days.map((item, index) => ({
          timestamp_ns: timeAt(index),
          price: item.equity?.text ?? "",
        })),
      },
      equityPriceLines:
        Number.isFinite(deposit) && deposit > 0
          ? [
              {
                value: String(record.costs.deposit),
                label: t("初始资金"),
                dashed: true,
                axisLabelVisible: false,
              },
            ]
          : undefined,
      drawdownSeries: {
        kind: "line" as const,
        points: drawdown.map((value, index) => ({
          timestamp_ns: timeAt(index),
          price: (value * 100).toFixed(2),
        })),
      },
      months: monthlyReturns(record.costs.deposit, detail.days),
      benchmark: (() => {
        const bench = record.data?.diagnostics?.benchmark as BenchmarkData | undefined;
        if (!bench?.days?.length || !bench.nav?.length || !Number.isFinite(deposit) || deposit <= 0)
          return null;
        const navByDay = new Map(bench.days.map((d, i) => [d, bench.nav[i]]));
        const values = detail.days.map(item => {
          const nav = navByDay.get(item.tradingDay);
          return nav === undefined ? null : nav * deposit;
        });
        if (!values.some(v => v !== null)) return null;
        return {
          totalReturn: bench.total_return ?? null,
          maxDrawdown: bench.max_drawdown ?? null,
          sharpe: bench.sharpe ?? null,
          values,
        };
      })(),
    };
  }, [detail, record]);
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (!detail || !record) return <p className="subtle">{t("正在读取结果…")}</p>;
  const summary = detail.summary!;
  if (!overview) {
    const failed = runState(summary.state) === "failed";
    const summaryReason = summary.error ? (diagnosticSummary(summary.error) ?? summary.error) : "";
    const missingData =
      failed && openDataset && summary.error
        ? parseDataGap(summary.error, { minutes: isMinutePeriod(summary.period) })
        : null;
    return (
      <div className="lab-result">
        <p role={failed ? "alert" : undefined}>
          {t(states[runState(summary.state)] ?? "未开始")}
          {summaryReason ? (
            <span title={summaryReason !== summary.error ? summary.error : undefined}>
              {` · ${summaryReason}`}
            </span>
          ) : (
            ""
          )}
          {missingData && (
            <button
              type="button"
              className="lab-link lab-skipped-action"
              onClick={() => openDataset!(dataGapParams(missingData))}
            >
              {t("去采集")}
            </button>
          )}
        </p>
        <RunDetails
          record={detail.record}
          openVersion={openVersion}
          openFactor={openFactor}
          openRun={openRun}
        />
      </div>
    );
  }
  const {
    holdout,
    rolling,
    splitMarkers,
    deposit,
    drawdown,
    equitySeries,
    equityPriceLines,
    drawdownSeries,
    months,
    benchmark,
  } = overview;
  const verdicts = evaluateRunVerdicts(summary, record, detail.days.length);
  const excluded = record.data.excluded as { bars: number; days: string[] } | undefined;
  const coverage = (record.data.minute_coverage ?? []) as {
    contract: string;
    missing: Record<string, number>;
    zero_volume_minutes: number;
    unchecked_days: string[];
  }[];
  const rejections = (record.data.rejections ?? []) as Rejection[];
  const refused = rejections.reduce((sum, row) => sum + row.count, 0);
  const tabs = [
    ["overview", t("概览")],
    ["diagnostics", t("归因与诊断")],
    ["bars", t("K 线与买卖点")],
    ["fills", t("成交（{count} 笔）", { count: detail.fills.length })],
    ["days", t("每日资金")],
    ["record", t("复现信息")],
  ] as const;
  return (
    <div className="lab-result">
      {excluded && excluded.bars > 0 && (
        <p className="subtle">
          {t("{bars} 根 K 线含不完整的分钟，未参与回测：{days}", {
            bars: excluded.bars,
            days: excluded.days.join(listSeparator()),
          })}
        </p>
      )}
      {coverage.map(row => {
        const missing = Object.entries(row.missing);
        return (
          <Fragment key={row.contract}>
            {missing.length > 0 && (
              <details>
                <summary>
                  {t("{contract} 有 {count} 个 K 线周期整段缺记录", {
                    contract: row.contract,
                    count: missing.reduce((sum, [, count]) => sum + count, 0),
                  })}
                </summary>
                <ul>
                  {missing.map(([dayStr, count]) => (
                    <li key={dayStr}>{t("{day} 缺 {count} 个周期", { day: dayStr, count })}</li>
                  ))}
                </ul>
              </details>
            )}
            {row.zero_volume_minutes > 0 && (
              <p className="subtle">
                {t("{contract} 有 {count} 条零成交分钟记录", {
                  contract: row.contract,
                  count: row.zero_volume_minutes,
                })}
              </p>
            )}
            {row.unchecked_days.length > 0 && (
              <p className="subtle">
                {t("{contract} 有待核实的交易时段：{days}", {
                  contract: row.contract,
                  days: row.unchecked_days.join(listSeparator()),
                })}
              </p>
            )}
          </Fragment>
        );
      })}
      {refused > 0 && (
        <details className="lab-rejections">
          <summary>
            {t("目标被风控或资金拒绝 {count} 次，实盘会告警并放弃当批，实例继续运行", {
              count: refused,
            })}
          </summary>
          <div className="lab-scroll">
            <table className="lab-table">
              <thead>
                <tr>
                  {(
                    [
                      ["合约", false],
                      ["目标手数", true],
                      ["原因", false],
                      ["次数", true],
                      ["首次", true],
                      ["末次", true],
                    ] as const
                  ).map(([label, numeric]) => (
                    <th scope="col" key={label} className={numeric ? "numeric" : undefined}>
                      {t(label)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rejections.map(row => (
                  <tr key={`${row.contract} ${row.lots} ${row.reason}`}>
                    <td>{row.contract}</td>
                    <td className="numeric">{row.lots}</td>
                    <td>{reasons[row.reason] ? t(reasons[row.reason]) : row.reason}</td>
                    <td className="numeric">{row.count}</td>
                    <td className="numeric">{minute(BigInt(row.first_ms) * 1_000_000n)}</td>
                    <td className="numeric">{minute(BigInt(row.last_ms) * 1_000_000n)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </details>
      )}
      <Figures summary={summary} previous={previous} />
      {verdicts.length > 0 && (
        <div className="lab-verdicts" role="list" aria-label={t("研究体检")}>
          {verdicts.map(item => (
            <div key={item.label} className="lab-verdict-item" role="listitem">
              <StatusBadge tone={item.tone}>{item.label}</StatusBadge>
              <span className="subtle">{item.detail}</span>
            </div>
          ))}
        </div>
      )}
      <ExperimentNoteBar
        id={summary.id}
        trailing={
          <CopySummaryButton
            build={() => formatRunSummaryMarkdown(detail, record, annotations[summary.id]?.note)}
          />
        }
      />
      {(holdout || rolling) && (
        <SegmentFigures
          summary={summary}
          segments={[
            ...(holdout
              ? ([
                  [t("留出日 {day} 之前", { day: holdout }), "development_"],
                  [t("留出段"), "holdout_"],
                ] as const)
              : []),
            ...(rolling ? ([[t("样本外（第一轮验证起）"), "out_of_sample_"]] as const) : []),
          ]}
        />
      )}
      <nav className="lab-report-tabs tabs" aria-label={t("报告")}>
        {tabs.map(([key, label]) => (
          <button type="button" key={key} aria-pressed={tab === key} onClick={() => setTab(key)}>
            {label}
          </button>
        ))}
      </nav>
      {tab === "overview" && (
        <>
          {benchmark && (
            <div className="lab-chart-controls">
              <label className="lab-check">
                <input
                  type="checkbox"
                  checked={showBenchmark}
                  onChange={event => setShowBenchmark(event.target.checked)}
                />
                {t("叠加等权基准")}
              </label>
              <span className="subtle numeric">
                {t("基准收益 {ret}", {
                  ret: metric(benchmark.totalReturn ?? undefined, true),
                })}
                {benchmark.totalReturn !== null &&
                  summary.metrics.total_return !== undefined &&
                  ` · ${t("超额 {excess}", {
                    excess: metric(summary.metrics.total_return - benchmark.totalReturn, true),
                  })}`}
              </span>
            </div>
          )}
          <PriceChart
            series={equitySeries}
            overlays={
              benchmark && showBenchmark
                ? [{ label: t("等权基准"), color: "var(--series-2)", values: benchmark.values }]
                : undefined
            }
            label={t("权益（逐日结算）")}
            titled
            volumeLabel={t("成交量")}
            showVolume={false}
            fillArea
            baselineValue={Number.isFinite(deposit) && deposit > 0 ? deposit : undefined}
            priceLines={equityPriceLines}
            verticalMarkers={splitMarkers}
            focusedTime={focusedDay}
            onFocusChange={setFocusedDay}
            formatTime={value => day(BigInt(value))}
            describe={index =>
              `${detail.days[index].tradingDay} · ${detail.days[index].equity?.text}`
            }
          />
          <PriceChart
            series={drawdownSeries}
            label={t("回撤（%）")}
            titled
            volumeLabel={t("成交量")}
            showVolume={false}
            fillArea
            baselineValue={0}
            verticalMarkers={splitMarkers}
            focusedTime={focusedDay}
            onFocusChange={setFocusedDay}
            formatTime={value => day(BigInt(value))}
            describe={index =>
              `${detail.days[index].tradingDay} · ${metric(drawdown[index], true)}`
            }
          />
          <h3>{t("月度收益")}</h3>
          <MonthlyReturns months={months} />
        </>
      )}
      {tab === "diagnostics" && (
        <RunDiagnostics
          id={summary.id}
          detail={detail}
          record={record}
          focusedDay={focusedDay}
          onFocusChange={setFocusedDay}
        />
      )}
      {tab === "bars" && (
        <RunChart detail={detail} data={record.data} contract={contract} choose={setContract} />
      )}
      {tab === "fills" && (
        <VirtualScrollTable
          label={t("成交（{count} 笔）", { count: detail.fills.length })}
          columns={8}
          headers={
            <tr>
              {(
                [
                  ["交易日", false],
                  ["时间", false],
                  ["合约", false],
                  ["方向", false],
                  ["开平", false],
                  ["手数", true],
                  ["价格", true],
                  ["手续费", true],
                ] as const
              ).map(([label, numeric]) => (
                <th scope="col" key={label} className={numeric ? "numeric" : undefined}>
                  {t(label)}
                </th>
              ))}
            </tr>
          }
          rows={detail.fills}
          renderRow={(fill, index) => (
            <tr key={fill.id || index}>
              <td>{fill.tradingDay}</td>
              <td>{minute(fill.timeMs * 1_000_000n)}</td>
              <td>{fill.contract}</td>
              <td>{fill.side === Side.BUY ? t("买") : t("卖")}</td>
              <td>{offsets[fill.offset] ? t(offsets[fill.offset]!) : "—"}</td>
              <td className="numeric">{groupedText(fill.lots)}</td>
              <td className="numeric">{fill.price?.text}</td>
              <td className="numeric">{groupedText(fill.fee)}</td>
            </tr>
          )}
        />
      )}
      {tab === "days" && (
        <VirtualScrollTable
          label={t("每日资金")}
          columns={5}
          headers={
            <tr>
              {["交易日", "权益", "结存", "手续费", "保证金"].map(label => (
                <th scope="col" key={label} className={label === "交易日" ? undefined : "numeric"}>
                  {t(label)}
                </th>
              ))}
            </tr>
          }
          rows={detail.days}
          renderRow={item => (
            <tr key={item.tradingDay}>
              <td>{item.tradingDay}</td>
              <td className="numeric">{groupedText(item.equity)}</td>
              <td className="numeric">{groupedText(item.balance)}</td>
              <td className="numeric">{groupedText(item.fees)}</td>
              <td className="numeric">{groupedText(item.margin)}</td>
            </tr>
          )}
        />
      )}
      {tab === "record" && (
        <RunDetails
          record={detail.record}
          openVersion={openVersion}
          openFactor={openFactor}
          openRun={openRun}
        />
      )}
    </div>
  );
}
