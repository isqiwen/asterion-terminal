import { Fragment, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  PriceChart,
  type Candle,
  type TradeMark,
  type VerticalMarker,
} from "@asterion/terminal/charts/PriceChart";
import { groupedText } from "../../src/bridge/decimal";
import { lab } from "../../src/bridge/lab";
import type {
  ChartBars,
  RunDetail,
  RunReport,
  RunSummary,
} from "../../src/generated/asterion/lab_pb";
import { useErrorNotification } from "../../src/host/notifications";
import { getLocale, type MessageValues, translate, listSeparator } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { warehouseName } from "../../src/ui/contract-names";
import { protocolPeriod } from "../../src/ui/periods";
import type { Tone } from "../../src/ui/StatusBadge";
import { useVirtualRows } from "../../src/ui/virtual-rows";
import { Offset, Period, Side } from "../../src/generated/asterion/common_pb";
import { NewModule, type ModuleDraft } from "./LabParts";
import {
  batchKey,
  batchStrategy,
  breakEvenSlippage,
  combinedStrategyStem,
  combinedStrategyTemplate,
  comparePortfolio,
  deflatedSharpeRatio,
  heatmap,
  medianValue,
  monthlyReturns,
  parameterText,
  runName,
  runState,
  slippageKey,
} from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The figures every run has, in the order the result shows them.
export const figures = [
  ["total_return", "总收益", true],
  ["annual_return", "年化收益", true],
  ["sharpe", "夏普比率", false],
  ["max_drawdown", "最大回撤", true],
  ["calmar", "卡玛比率", false],
  ["annual_volatility", "年化波动", true],
  ["winning_days", "上涨日占比", true],
] as const;

const prefixes: Record<string, string> = {
  development_: "留出前",
  holdout_: "留出段",
  out_of_sample_: "样本外",
};

// A metric's name as the page shows it: 夏普比率, 留出前 夏普比率, …
export function metricLabel(name: string) {
  const prefix = Object.keys(prefixes).find(item => name.startsWith(item)) ?? "";
  const base = figures.find(([key]) => key === name.slice(prefix.length));
  if (!base) return name;
  return prefix ? `${t(prefixes[prefix])} ${t(base[1])}` : t(base[1]);
}

const percentMetric = (name: string) => /return|drawdown|volatility|winning/.test(name);

// Gains and losses take the quote colours.
export function signed(value: number | undefined | null) {
  if (value === undefined || value === null || value === 0) return "";
  return value > 0 ? "lab-up" : "lab-down";
}

export function metric(value: number | undefined, percent: boolean) {
  if (value === undefined) return "—";
  return percent ? `${(value * 100).toFixed(2)}%` : value.toFixed(3);
}

export const periodText = (period: Period) =>
  period === Period.DAY
    ? t("日线")
    : t("{count} 分钟", { count: Period[period]?.replace("MINUTES_", "") ?? "?" });

// A moment as month-day and time in Beijing time.
export function shortTime(ms: bigint) {
  return new Date(Number(ms))
    .toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    .slice(5, 16);
}

// When a minute bar ended, Beijing time.
function minute(timeNs: bigint) {
  return new Date(Number(timeNs / 1_000_000n) + 8 * 3600_000)
    .toISOString()
    .slice(5, 16)
    .replace("T", " ");
}

function day(timeNs: bigint) {
  // Settlements and bars are labelled at 15:00 Beijing time.
  return new Date(Number(timeNs / 1_000_000n) + 8 * 3600_000).toISOString().slice(0, 10);
}

function Figures({ summary, prefix = "" }: { summary: RunSummary; prefix?: string }) {
  return (
    <dl className="lab-figures">
      {figures.map(([name, label, percent]) => {
        const value = summary.metrics[prefix + name];
        return (
          <div key={name}>
            <dt>{t(label)}</dt>
            <dd className={name.endsWith("return") ? signed(value) : undefined}>
              {metric(value, percent)}
            </dd>
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

function VirtualScrollTable<T>({
  label,
  columns,
  headers,
  rows,
  renderRow,
}: {
  label: string;
  columns: number;
  headers: ReactNode;
  rows: readonly T[];
  renderRow: (row: T, index: number) => ReactNode;
}) {
  const parentRef = useRef<HTMLDivElement>(null);
  const { start, end, topPad, bottomPad } = useVirtualRows(parentRef, rows.length);
  return (
    <div
      ref={parentRef}
      className="lab-virtual-scroll"
      tabIndex={0}
      role="region"
      aria-label={label}
    >
      <table className="lab-table">
        <thead>{headers}</thead>
        <tbody>
          {topPad > 0 && (
            <tr data-spacer aria-hidden="true">
              <td colSpan={columns} style={{ height: topPad }} />
            </tr>
          )}
          {rows.slice(start, end).map((row, offset) => renderRow(row, start + offset))}
          {bottomPad > 0 && (
            <tr data-spacer aria-hidden="true">
              <td colSpan={columns} style={{ height: bottomPad }} />
            </tr>
          )}
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
    const bars = data.bars;
    const minutes = bars.period === "day" ? 0 : Number(String(bars.period).replace("m", ""));
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

// One run's result: its figures, equity, trades on the bars, months and what
// reproduces it. Read again when `state`, the run's state in the list of
// runs, changes, so a running run shows its result once it has ended.
// `openVersion` shows a research dataset version on the data page.
export function RunResult({
  id,
  state,
  openVersion,
}: {
  id: string;
  state: string;
  openVersion?: (version: string) => void;
}) {
  const [detail, setDetail] = useState<RunDetail | null>(null);
  const [contract, setContract] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [tab, setTab] = useState<
    "overview" | "diagnostics" | "bars" | "fills" | "days" | "quantstats" | "record"
  >("overview");
  const [focusedDay, setFocusedDay] = useState<string | null>(null);
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
      return peak > 0 ? Math.max(-1, value / peak - 1) : 0;
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
    };
  }, [detail, record]);
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (!detail || !record) return <p className="subtle">{t("正在读取结果…")}</p>;
  const summary = detail.summary!;
  if (!overview)
    return (
      <div className="lab-result">
        <p role={runState(summary.state) === "failed" ? "alert" : undefined}>
          {t(states[runState(summary.state)] ?? "未开始")}
          {summary.error ? ` · ${summary.error}` : ""}
        </p>
        <RunDetails record={detail.record} openVersion={openVersion} />
      </div>
    );
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
  } = overview;
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
    ["quantstats", t("QuantStats 指标")],
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
                  {missing.map(([day, count]) => (
                    <li key={day}>{t("{day} 缺 {count} 个周期", { day, count })}</li>
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
      <Figures summary={summary} />
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
          <PriceChart
            series={equitySeries}
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
      {tab === "diagnostics" && <RunDiagnostics detail={detail} record={record} />}
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
      {tab === "quantstats" && <QuantStats id={id} />}
      {tab === "record" && <RunDetails record={detail.record} openVersion={openVersion} />}
    </div>
  );
}

// Each month's return, a year to a row, with the year's compounded total.
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

type DiagnosticsData = {
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

function RunDiagnostics({
  detail,
  record,
}: {
  detail: RunDetail;
  record: { costs?: { deposit?: string }; data?: { diagnostics?: DiagnosticsData } };
}) {
  const diag = useMemo<DiagnosticsData>(() => {
    if (record.data?.diagnostics) return record.data.diagnostics;
    // Fallback for runs recorded before diagnostics were persisted.
    const deposit = Number(record.costs?.deposit ?? 0);
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
  }, [detail.days, record]);

  const trades = diag.trades ?? {};
  const costs = diag.costs ?? {};
  const exposure = diag.exposure ?? {};
  const direction = diag.direction ?? {};
  const bySymbol = diag.by_symbol ?? [];
  const bySector = diag.by_sector ?? [];
  const components = diag.components ?? [];
  const sectorName = (id: string) => t(sectorLabels[id] ?? "其他");
  const symbolLabel = (symbol: string) => {
    const name = warehouseName(symbol);
    return name ? `${name} ${symbol}` : symbol;
  };
  return (
    <>
      <h3>{t("交易质量")}</h3>
      <dl className="lab-figures">
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

      <h3>{t("成本与方向")}</h3>
      <dl className="lab-figures">
        <div>
          <dt>{t("毛盈亏")}</dt>
          <dd className={signed(costs.gross_pnl)}>{money(costs.gross_pnl)}</dd>
        </div>
        <div>
          <dt>{t("手续费")}</dt>
          <dd>{money(costs.fees)}</dd>
        </div>
        <div>
          <dt>{t("估算滑点")}</dt>
          <dd>{money(costs.estimated_slippage)}</dd>
        </div>
        <div>
          <dt>{t("净盈亏")}</dt>
          <dd className={signed(costs.net_pnl)}>{money(costs.net_pnl)}</dd>
        </div>
        <div>
          <dt>{t("多头盈亏")}</dt>
          <dd className={signed(direction.long_pnl)}>{money(direction.long_pnl)}</dd>
        </div>
        <div>
          <dt>{t("空头盈亏")}</dt>
          <dd className={signed(direction.short_pnl)}>{money(direction.short_pnl)}</dd>
        </div>
      </dl>

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
    </>
  );
}
function MonthlyReturns({ months }: { months: { month: string; value: number }[] }) {
  const years = [...new Set(months.map(item => item.month.slice(0, 4)))];
  const of = new Map(months.map(item => [item.month, item.value]));
  return (
    <div className="lab-scroll">
      <table className="lab-table lab-monthly" aria-label={t("月度收益")}>
        <thead>
          <tr>
            <th scope="col">
              <span className="sr-only">{t("年份")}</span>
            </th>
            {Array.from({ length: 12 }, (_, index) => (
              <th scope="col" key={index} className="numeric">
                {index + 1}
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
            return (
              <tr key={year}>
                <th scope="row">{year}</th>
                {values.map((value, index) => (
                  <td key={index} className={`numeric ${signed(value)}`}>
                    {value === undefined ? "" : metric(value, true)}
                  </td>
                ))}
                <td className={`numeric ${signed(total)}`}>{metric(total, true)}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
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
function QuantStats({ id }: { id: string }) {
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
            QuantStats {report.quantstatsVersion} · {t("日收益 · 无风险利率 0 · 一年 252 期")}
          </p>
        </>
      )}
    </div>
  );
}

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
export const states: Record<string, string> = {
  running: "运行中",
  succeeded: "已完成",
  failed: "失败",
  stopped: "已停止",
  interrupted: "已中断",
};
export const runTones: Record<string, Tone> = {
  succeeded: "ok",
  failed: "danger",
  interrupted: "attention",
};

// What reproduces the run: strategy version, data, engine, costs and limits.
function RunDetails({
  record,
  openVersion,
}: {
  record: string;
  openVersion?: (version: string) => void;
}) {
  const value = JSON.parse(record);
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
      </dl>
      <pre>
        {JSON.stringify({ params: value.params, costs: value.costs, risk: value.risk }, null, 2)}
      </pre>
    </div>
  );
}

// Colours of the compared runs after the first, which takes the chart's own.
const overlayColors = [
  "var(--series-1)",
  "var(--series-3)",
  "var(--series-4)",
  "var(--series-5)",
  "var(--series-2)",
];

// Runs side by side: their figures, the parameters that differ, their net
// value on the first run's days, correlation matrix, and portfolio synthesis.
export function RunComparison({
  ids,
  repository,
  onGenerated,
}: {
  ids: readonly string[];
  repository?: string;
  onGenerated?: (
    path: string,
    id: string,
    scope: { symbols: string; begin: string; end: string },
  ) => void;
}) {
  return (
    <Comparison
      key={ids.join("\n")}
      joined={ids.join("\n")}
      repository={repository}
      onGenerated={onGenerated}
    />
  );
}

// `joined` is the run ids, one a line: a parent's new array of the same ids reads nothing again.
function Comparison({
  joined,
  repository,
  onGenerated,
}: {
  joined: string;
  repository?: string;
  onGenerated?: (
    path: string,
    id: string,
    scope: { symbols: string; begin: string; end: string },
  ) => void;
}) {
  const ids = joined.split("\n");
  const [details, setDetails] = useState<RunDetail[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [simMode, setSimMode] = useState<"equal" | "inv_vol">("equal");
  const [generating, setGenerating] = useState<ModuleDraft | null>(null);
  useEffect(() => {
    let active = true;
    void Promise.all(joined.split("\n").map(id => lab({ case: "getRun", value: { id } })))
      .then(replies => active && setDetails(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [joined]);
  const portfolio = useMemo(
    () => (details.length === ids.length ? comparePortfolio(details, simMode) : null),
    [details, ids.length, simMode],
  );
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (details.length !== ids.length) return <p className="subtle">{t("正在读取结果…")}</p>;
  const [first, ...others] = details;
  const name = (detail: RunDetail) => runName(detail.summary!);
  const base = (detail: RunDetail) => Number(JSON.parse(detail.record).costs.deposit);
  const seriesColor = (index: number) =>
    index === 0 ? "var(--price-line)" : overlayColors[(index - 1) % overlayColors.length];
  const rangeOf = (detail: RunDetail) =>
    detail.summary!.firstDay || detail.summary!.lastDay
      ? `${detail.summary!.firstDay} – ${detail.summary!.lastDay}`
      : "";
  const contextRows = (
    [
      [t("策略"), (detail: RunDetail) => detail.summary!.strategy],
      [t("品种"), (detail: RunDetail) => detail.summary!.symbols.join(listSeparator())],
      [t("区间"), rangeOf],
    ] as const
  ).filter(([, read]) => new Set(details.map(read)).size > 1);
  const diffRange = new Set(details.map(rangeOf)).size > 1;
  const params = [...new Set(details.flatMap(detail => Object.keys(detail.summary!.params)))];
  const differing = params.filter(
    param => new Set(details.map(detail => parameterText(detail.summary!.params[param]))).size > 1,
  );
  const distinctStrategies = new Set(details.map(d => d.summary!.strategy).filter(Boolean));
  const resolvedRepo =
    repository ||
    (details
      .map(
        d => (JSON.parse(d.record) as { strategy?: { repository?: string } }).strategy?.repository,
      )
      .find(Boolean) ??
      "");
  const canGenerateCombined = Boolean(
    portfolio && resolvedRepo && distinctStrategies.size >= 2 && onGenerated,
  );
  const portfolioByDay = portfolio
    ? new Map(portfolio.days.map((d, idx) => [d, portfolio.nav[idx]!]))
    : null;
  const corrCellStyle = (value: number | null) => {
    if (value === null) return undefined;
    const strength = Math.round(Math.min(1, Math.abs(value)) * 45);
    const tone = value >= 0 ? "var(--quote-up)" : "var(--quote-down)";
    return { background: `color-mix(in oklab, ${tone} ${strength}%, var(--panel))` };
  };
  return (
    <div className="lab-result">
      <div className="lab-scroll">
        <table className="lab-table lab-compare">
          <thead>
            <tr>
              <th scope="col">
                <span className="sr-only">{t("指标")}</span>
              </th>
              {details.map((detail, index) => (
                <th scope="col" key={detail.summary!.id} className="numeric">
                  <span
                    className="lab-swatch"
                    aria-hidden="true"
                    style={{ background: seriesColor(index) }}
                  />
                  {name(detail)}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {[...figures, ["fills", "成交笔数", false] as const].map(
              ([metricName, label, percent]) => (
                <tr key={metricName}>
                  <td>{t(label)}</td>
                  {details.map(detail => (
                    <td
                      key={detail.summary!.id}
                      className={`numeric ${metricName.endsWith("return") ? signed(detail.summary!.metrics[metricName]) : ""}`}
                    >
                      {metricName === "fills"
                        ? (detail.summary!.metrics.fills ?? "—")
                        : metric(detail.summary!.metrics[metricName], percent)}
                    </td>
                  ))}
                </tr>
              ),
            )}
            {contextRows.map(([label, read]) => (
              <tr key={label} className="lab-differs">
                <td>{label}</td>
                {details.map(detail => (
                  <td key={detail.summary!.id} className="numeric">
                    {read(detail) || "—"}
                  </td>
                ))}
              </tr>
            ))}
            {differing.map(param => (
              <tr key={param} className="lab-differs">
                <td>{param}</td>
                {details.map(detail => (
                  <td key={detail.summary!.id} className="numeric">
                    {parameterText(detail.summary!.params[param])}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {diffRange && (
        <p className="subtle">
          {t("净值曲线按首次运行（{name}）的交易日对齐", { name: name(first) })}
        </p>
      )}
      <PriceChart
        series={{
          kind: "line",
          points: first.days.map(item => ({
            timestamp_ns: String(item.settledMs * 1_000_000n),
            price: String(Number(item.equity?.text) / base(first)),
          })),
        }}
        overlays={[
          ...others.map((detail, index) => {
            const equity = new Map(
              detail.days.map(item => [item.tradingDay, Number(item.equity?.text)]),
            );
            return {
              label: name(detail),
              color: overlayColors[index % overlayColors.length]!,
              values: first.days.map(item => {
                const value = equity.get(item.tradingDay);
                return value === undefined ? null : value / base(detail);
              }),
            };
          }),
          ...(portfolioByDay
            ? [
                {
                  label: t("组合试算"),
                  color: "var(--accent)",
                  values: first.days.map(item => portfolioByDay.get(item.tradingDay) ?? null),
                },
              ]
            : []),
        ]}
        label={t("净值：{names}", { names: details.map(name).join(listSeparator()) })}
        titled
        volumeLabel={t("成交量")}
        showVolume={false}
        formatTime={value => day(BigInt(value))}
        describe={index =>
          first.days[index]
            ? `${first.days[index].tradingDay} · ${name(first)}: ${(Number(first.days[index].equity?.text) / base(first)).toFixed(4)}`
            : null
        }
      />
      {portfolio && (
        <>
          <div className="lab-toolbar">
            <h3>{t("多策略组合试算")}</h3>
            <div className="lab-segmented" role="group" aria-label={t("合成方式")}>
              {(
                [
                  ["equal", "等权合成"],
                  ["inv_vol", "波动率平价"],
                ] as const
              ).map(([mode, label]) => (
                <button
                  key={mode}
                  type="button"
                  aria-pressed={simMode === mode}
                  onClick={() => setSimMode(mode)}
                >
                  {t(label)}
                </button>
              ))}
            </div>
            <span className="lab-spacer" />
            {canGenerateCombined && (
              <button
                type="button"
                className="primary"
                onClick={() =>
                  setGenerating({
                    title: "生成组合策略",
                    name: combinedStrategyStem(details),
                    build: moduleName =>
                      combinedStrategyTemplate(
                        details,
                        portfolio.weights,
                        simMode === "inv_vol",
                        moduleName,
                      ),
                  })
                }
              >
                {t("生成组合策略")}
              </button>
            )}
          </div>
          <dl className="lab-figures">
            <div>
              <dt>{t("组合总收益")}</dt>
              <dd className={signed(portfolio.totalReturn)}>
                {metric(portfolio.totalReturn, true)}
              </dd>
            </div>
            <div>
              <dt>{t("组合夏普")}</dt>
              <dd>{metric(portfolio.sharpe ?? undefined, false)}</dd>
            </div>
            <div>
              <dt>{t("组合最大回撤")}</dt>
              <dd>{metric(portfolio.maxDrawdown, true)}</dd>
            </div>
            <div>
              <dt>{t("平均相关性")}</dt>
              <dd>{metric(portfolio.avgCorrelation ?? undefined, false)}</dd>
            </div>
            <div>
              <dt>{t("波动降幅")}</dt>
              <dd className={signed(portfolio.volReduction)}>
                {metric(portfolio.volReduction ?? undefined, true)}
              </dd>
            </div>
          </dl>
          <p className="subtle">
            {details
              .map(
                (detail, idx) => `${name(detail)} ${(portfolio.weights[idx]! * 100).toFixed(1)}%`,
              )
              .join(" · ")}
          </p>
          <h3>{t("策略相关性矩阵")}</h3>
          <div className="lab-scroll">
            <table className="lab-heatmap lab-corr-heatmap" aria-label={t("策略相关性矩阵")}>
              <thead>
                <tr>
                  <th scope="col" />
                  {details.map(detail => (
                    <th scope="col" key={detail.summary!.id}>
                      {name(detail)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {details.map((rowDetail, r) => (
                  <tr key={rowDetail.summary!.id}>
                    <th scope="row">{name(rowDetail)}</th>
                    {portfolio.correlations[r]!.map((val, c) => (
                      <td key={c} style={corrCellStyle(val)}>
                        {val === null ? "—" : val.toFixed(3)}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {generating && resolvedRepo && (
            <NewModule
              hideTrigger
              folder="strategies"
              repository={resolvedRepo}
              generated={generating}
              cancelGenerated={() => setGenerating(null)}
              created={(path, id) => {
                const symbols = [...new Set(details.flatMap(d => d.summary!.symbols))].join(", ");
                const begin = portfolio.days[0] ?? "";
                const end = portfolio.days.at(-1) ?? "";
                onGenerated?.(path, id ?? "", { symbols, begin, end });
              }}
            />
          )}
        </>
      )}
    </div>
  );
}

// A sweep's runs ranked by a metric, and with two swept dimensions a grid of it.
export function SweepResult({
  runs,
  selected,
  open,
  rolling,
}: {
  runs: readonly RunSummary[];
  selected: string;
  open: (id: string) => void;
  rolling?: () => void;
}) {
  const names = Object.keys(runs[0]?.params ?? {});
  const hasSymbols = new Set(runs.map(run => run.symbols.join(","))).size > 1;
  const symbolAxis = "__symbol__";
  const formatSymbols = (symbols: readonly string[]) =>
    symbols
      .map(symbol => {
        const label = warehouseName(symbol);
        return label ? `${label} ${symbol}` : symbol;
      })
      .join(listSeparator());
  const [measure, setMeasure] = useState<string>(
    runs.some(run => "development_sharpe" in run.metrics) ? "development_sharpe" : "sharpe",
  );
  const rows = runs.map(run => ({
    id: run.id,
    state: runState(run.state),
    symbols: formatSymbols(run.symbols),
    span: run.firstDay && run.lastDay ? `${run.firstDay} – ${run.lastDay}` : "",
    metrics: run.metrics,
    params: Object.fromEntries([
      ...(hasSymbols ? [[symbolAxis, formatSymbols(run.symbols)]] : []),
      ...names.map(name => [
        name,
        name === batchKey ? batchStrategy(run).split(".")[1] : parameterText(run.params[name]),
      ]),
    ]),
    metric: run.metrics[measure] ?? null,
  }));
  const lowerBetter = /drawdown|volatility|ulcer/.test(measure);
  const ranked = [...rows].sort((a, b) => {
    if (a.metric === null) return 1;
    if (b.metric === null) return -1;
    return lowerBetter ? a.metric - b.metric : b.metric - a.metric;
  });
  const sweptParams = names.filter(name => new Set(rows.map(row => row.params[name])).size > 1);
  const sweptAxes = [...(hasSymbols ? [symbolAxis] : []), ...sweptParams];
  const axisLabel = (axis: string) =>
    axis === symbolAxis
      ? t("品种")
      : axis === batchKey
        ? t("策略")
        : axis === slippageKey
          ? t("滑点（跳）")
          : axis;
  const isSlippage = names.length === 1 && names[0] === slippageKey;
  const grid = sweptAxes.length === 2 ? heatmap(rows, sweptAxes[0]!, sweptAxes[1]!) : null;
  const cellRuns = grid
    ? new Map(rows.map(row => [`${row.params[sweptAxes[0]!]}\0${row.params[sweptAxes[1]!]}`, row]))
    : null;
  const values = rows.flatMap(row => (row.metric === null ? [] : [row.metric]));
  const low = Math.min(...values),
    high = Math.max(...values);
  const percent = percentMetric(measure);
  const pureMultiSymbol = (hasSymbols && names.length === 0) || names[0] === batchKey || isSlippage;
  const extraMeasure = !["total_return", "sharpe", "max_drawdown"].includes(measure);

  const slippageSummary = isSlippage
    ? breakEvenSlippage(
        runs
          .filter(run => runState(run.state) === "succeeded")
          .map(run => ({
            ticks: Number(parameterText(run.params[slippageKey])),
            sharpe: run.metrics.sharpe,
            totalReturn: run.metrics.total_return,
          })),
      )
    : null;

  // Parameter sweep overfitting check: Deflated Sharpe Ratio & immediate grid neighbor median.
  const sweepCheck = (() => {
    if (pureMultiSymbol || isSlippage) return null;
    const sharpeKey = runs.some(r => "development_sharpe" in r.metrics)
      ? "development_sharpe"
      : "sharpe";
    const sharpes = runs
      .map(r => r.metrics[sharpeKey])
      .filter((v): v is number => v !== undefined && Number.isFinite(v));
    if (sharpes.length < 2) return null;
    const bestSharpe = Math.max(...sharpes);
    const sampleRun = runs.find(r => r.firstDay && r.lastDay);
    const daysEstimate = sampleRun
      ? Math.max(
          20,
          Math.round(
            ((Date.parse(sampleRun.lastDay) - Date.parse(sampleRun.firstDay)) / 86400000) *
              (252 / 365),
          ),
        )
      : 252;
    const dsr = deflatedSharpeRatio(bestSharpe, sharpes, daysEstimate);

    // Plateau robustness: median Sharpe of immediate grid neighbors around the best parameter cell.
    let neighborMedian: number | null = null;
    if (sweptAxes.length === 2) {
      const sGrid = heatmap(
        rows.map(row => ({ params: row.params, metric: row.metrics[sharpeKey] ?? null })),
        sweptAxes[0]!,
        sweptAxes[1]!,
      );
      let bestRow = -1;
      let bestCol = -1;
      let bestVal = -Infinity;
      sGrid.cells.forEach((rowCells, r) =>
        rowCells.forEach((val, c) => {
          if (val !== null && val > bestVal) {
            bestVal = val;
            bestRow = r;
            bestCol = c;
          }
        }),
      );
      if (bestRow >= 0 && bestCol >= 0) {
        const neighbors: number[] = [];
        for (let dr = -1; dr <= 1; dr++) {
          for (let dc = -1; dc <= 1; dc++) {
            if (dr === 0 && dc === 0) continue;
            const v = sGrid.cells[bestRow + dr]?.[bestCol + dc];
            if (v !== null && v !== undefined && Number.isFinite(v)) neighbors.push(v);
          }
        }
        neighborMedian = medianValue(neighbors);
      }
    } else if (sweptAxes.length === 1) {
      const axis = sweptAxes[0]!;
      const ordered = [...rows]
        .filter(row => row.metrics[sharpeKey] !== undefined)
        .sort((a, b) => Number(a.params[axis]) - Number(b.params[axis]));
      let bestIdx = -1;
      let bestVal = -Infinity;
      ordered.forEach((row, idx) => {
        const v = row.metrics[sharpeKey]!;
        if (v > bestVal) {
          bestVal = v;
          bestIdx = idx;
        }
      });
      if (bestIdx >= 0) {
        const neighbors = [ordered[bestIdx - 1], ordered[bestIdx + 1]]
          .filter((item): item is (typeof ordered)[number] => Boolean(item))
          .map(item => item.metrics[sharpeKey]!);
        neighborMedian = medianValue(neighbors);
      }
    }
    const plateauRatio =
      neighborMedian !== null && bestSharpe > 0 ? neighborMedian / bestSharpe : null;
    return { bestSharpe, dsr, neighborMedian, plateauRatio };
  })();

  return (
    <div className="lab-result">
      {slippageSummary && (
        <dl className="lab-figures" aria-label={t("滑点压力测试")}>
          <div>
            <dt>{t("盈亏平衡滑点")}</dt>
            <dd>
              {slippageSummary.status === "above_max"
                ? t("> {ticks} 跳", { ticks: slippageSummary.maxTicks })
                : slippageSummary.status === "interpolated" && slippageSummary.ticks !== null
                  ? t("{ticks} 跳", { ticks: slippageSummary.ticks.toFixed(1) })
                  : t("无正收益")}
            </dd>
          </div>
          {runs
            .slice()
            .sort(
              (a, b) =>
                Number(parameterText(a.params[slippageKey])) -
                Number(parameterText(b.params[slippageKey])),
            )
            .map(run => (
              <div key={run.id}>
                <dt>{t("{ticks} 跳", { ticks: parameterText(run.params[slippageKey]) })}</dt>
                <dd className={signed(run.metrics.total_return)}>
                  {run.metrics.sharpe !== undefined
                    ? metric(run.metrics.sharpe, false)
                    : metric(run.metrics.total_return, true)}
                </dd>
              </div>
            ))}
        </dl>
      )}
      {sweepCheck && (
        <dl className="lab-figures" aria-label={t("过拟合体检")}>
          <div>
            <dt>{t("最佳夏普")}</dt>
            <dd>{metric(sweepCheck.bestSharpe, false)}</dd>
          </div>
          <div>
            <dt>{t("检验组数")}</dt>
            <dd>{sweepCheck.dsr?.trials ?? runs.length}</dd>
          </div>
          <div>
            <dt>{t("DSR 置信度")}</dt>
            <dd>{metric(sweepCheck.dsr?.dsr, true)}</dd>
          </div>
          <div>
            <dt>{t("邻域中位")}</dt>
            <dd>{metric(sweepCheck.neighborMedian ?? undefined, false)}</dd>
          </div>
          <div>
            <dt>{t("高原稳健度")}</dt>
            <dd>{metric(sweepCheck.plateauRatio ?? undefined, true)}</dd>
          </div>
        </dl>
      )}
      <div className="lab-toolbar">
        <label className="lab-inline">
          {t("按指标")}
          <select value={measure} onChange={event => setMeasure(event.target.value)}>
            {[...new Set(runs.flatMap(run => Object.keys(run.metrics)))].sort().map(name => (
              <option key={name} value={name}>
                {metricLabel(name)}
              </option>
            ))}
          </select>
        </label>
        {rolling && (
          <button type="button" className="lab-link" onClick={rolling}>
            {t("查看滚动验证的账户")}
          </button>
        )}
      </div>
      {grid && (
        <div className="lab-scroll" tabIndex={0} role="region" aria-label={t("参数热力图")}>
          <table className="lab-heatmap" aria-label={t("参数热力图")}>
            <thead>
              <tr>
                <th scope="col">
                  {axisLabel(sweptAxes[0]!)} \ {axisLabel(sweptAxes[1]!)}
                </th>
                {grid.columns.map(column => (
                  <th scope="col" key={column}>
                    {column}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {grid.rows.map((row, r) => (
                <tr key={row}>
                  <th scope="row">{row}</th>
                  {grid.cells[r].map((cell, c) => {
                    const column = grid.columns[c]!;
                    const match = cellRuns?.get(`${row}\0${column}`);
                    const text = cell === null ? "—" : metric(cell, percent);
                    const ratio =
                      cell === null || high === low
                        ? null
                        : lowerBetter
                          ? (high - cell) / (high - low)
                          : (cell - low) / (high - low);
                    return (
                      <td
                        key={c}
                        className={match?.id === selected ? "selected" : undefined}
                        style={
                          ratio === null
                            ? undefined
                            : {
                                background: `color-mix(in oklab, var(--accent) ${Math.round(12 + 56 * ratio)}%, var(--panel))`,
                              }
                        }
                      >
                        {match?.state === "succeeded" ? (
                          <button
                            type="button"
                            className="lab-heatmap-cell"
                            title={`${axisLabel(sweptAxes[0]!)}=${row}, ${axisLabel(sweptAxes[1]!)}=${column} · ${metricLabel(measure)}: ${text}`}
                            onClick={() => open(match.id)}
                          >
                            {text}
                          </button>
                        ) : (
                          text
                        )}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <VirtualScrollTable
        label={t("排名")}
        columns={
          1 +
          (hasSymbols ? 1 : 0) +
          names.length +
          (pureMultiSymbol ? 1 : 0) +
          3 +
          (extraMeasure ? 1 : 0) +
          1
        }
        headers={
          <tr>
            <th scope="col">{t("排名")}</th>
            {hasSymbols && <th scope="col">{t("品种")}</th>}
            {names.map(name => (
              <th scope="col" key={name}>
                {axisLabel(name)}
              </th>
            ))}
            {pureMultiSymbol && <th scope="col">{t("区间")}</th>}
            <th scope="col" className="numeric">
              {metricLabel("total_return")}
            </th>
            <th scope="col" className="numeric">
              {metricLabel("sharpe")}
            </th>
            <th scope="col" className="numeric">
              {metricLabel("max_drawdown")}
            </th>
            {extraMeasure && (
              <th scope="col" className="numeric">
                {metricLabel(measure)}
              </th>
            )}
            <th scope="col">{t("状态")}</th>
          </tr>
        }
        rows={ranked}
        renderRow={(row, index) => (
          <tr
            key={row.id}
            className={row.id === selected ? "selected" : undefined}
            data-openable={row.state === "succeeded" || undefined}
            onClick={row.state === "succeeded" ? () => open(row.id) : undefined}
          >
            <td>{index + 1}</td>
            {hasSymbols && <td>{row.symbols}</td>}
            {names.map(name => (
              <td key={name}>{row.params[name]}</td>
            ))}
            {pureMultiSymbol && <td className="subtle">{row.span || "—"}</td>}
            <td className={`numeric ${signed(row.metrics.total_return)}`}>
              {metric(row.metrics.total_return, true)}
            </td>
            <td className="numeric">{metric(row.metrics.sharpe, false)}</td>
            <td className="numeric">{metric(row.metrics.max_drawdown, true)}</td>
            {extraMeasure && (
              <td className="numeric">{row.metric === null ? "—" : metric(row.metric, percent)}</td>
            )}
            <td>
              {row.state === "succeeded" ? (
                <button
                  type="button"
                  className="lab-link"
                  onClick={event => {
                    event.stopPropagation();
                    open(row.id);
                  }}
                >
                  {t("查看")}
                </button>
              ) : (
                t(states[row.state] ?? "未开始")
              )}
              {row.id === selected ? ` · ${t("选中")}` : ""}
            </td>
          </tr>
        )}
      />
    </div>
  );
}
