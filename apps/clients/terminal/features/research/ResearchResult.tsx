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
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { warehouseName } from "../../src/ui/contract-names";
import { protocolPeriod } from "../../src/ui/periods";
import type { Tone } from "../../src/ui/StatusBadge";
import { useVirtualRows } from "../../src/ui/virtual-rows";
import { Offset, Period, Side } from "../../src/generated/asterion/common_pb";
import {
  batchKey,
  batchStrategy,
  heatmap,
  monthlyReturns,
  parameterText,
  runName,
  runState,
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
  const contracts = [...new Set(detail.fills.map(fill => fill.contract))];
  const daily = data.bars.period === "day";
  const candles: Candle[] = bars.map(bar => ({
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
  const trades: TradeMark[] = detail.fills
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
    });
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
      {error && <button onClick={() => setRetry(value => value + 1)}>{t("重试")}</button>}
      {candles.length > 0 && (
        <PriceChart
          series={{ kind: "candles", points: candles }}
          label={contract}
          volumeLabel={t("成交量")}
          trades={trades.filter(trade => trade.index >= 0)}
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
  const [tab, setTab] = useState<"overview" | "bars" | "fills" | "days" | "quantstats" | "record">(
    "overview",
  );
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
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (!detail || !record) return <p className="subtle">{t("正在读取结果…")}</p>;
  const summary = detail.summary!;
  if (runState(summary.state) !== "succeeded")
    return (
      <div className="lab-result">
        <p role={runState(summary.state) === "failed" ? "alert" : undefined}>
          {t(states[runState(summary.state)] ?? "未开始")}
          {summary.error ? ` · ${summary.error}` : ""}
        </p>
        <RunDetails record={detail.record} openVersion={openVersion} />
      </div>
    );
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
  const excluded = record.data.excluded as { bars: number; days: string[] } | undefined;
  const coverage = (record.data.minute_coverage ?? []) as {
    contract: string;
    missing: Record<string, number>;
    zero_volume_minutes: number;
    unchecked_days: string[];
  }[];
  const rejections = (record.data.rejections ?? []) as Rejection[];
  const refused = rejections.reduce((sum, row) => sum + row.count, 0);
  const equity = detail.days.map(item => Number(item.equity?.text));
  const deposit = Number(record.costs?.deposit);
  let peak = Number.isFinite(deposit) && deposit > 0 ? deposit : -Infinity;
  const drawdown = equity.map(value => {
    peak = Math.max(peak, value);
    return peak > 0 ? Math.max(-1, value / peak - 1) : 0;
  });
  const time = (index: number) => String(detail.days[index].settledMs * 1_000_000n);
  const tabs = [
    ["overview", t("概览")],
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
                  {["合约", "目标手数", "原因", "次数", "首次", "末次"].map(label => (
                    <th scope="col" key={label}>
                      {t(label)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rejections.map(row => (
                  <tr key={`${row.contract} ${row.lots} ${row.reason}`}>
                    <td>{row.contract}</td>
                    <td>{row.lots}</td>
                    <td>{reasons[row.reason] ? t(reasons[row.reason]) : row.reason}</td>
                    <td>{row.count}</td>
                    <td>{minute(BigInt(row.first_ms) * 1_000_000n)}</td>
                    <td>{minute(BigInt(row.last_ms) * 1_000_000n)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </details>
      )}
      <Figures summary={summary} />
      {holdout && (
        <div className="lab-segments">
          <div>
            <h3>{t("留出日 {day} 之前", { day: holdout })}</h3>
            <Figures summary={summary} prefix="development_" />
          </div>
          <div>
            <h3>{t("留出段")}</h3>
            <Figures summary={summary} prefix="holdout_" />
          </div>
        </div>
      )}
      {rolling && (
        <>
          <h3>{t("样本外（第一轮验证起）")}</h3>
          <Figures summary={summary} prefix="out_of_sample_" />
        </>
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
            series={{
              kind: "line",
              points: detail.days.map((item, index) => ({
                timestamp_ns: time(index),
                price: item.equity?.text ?? "",
              })),
            }}
            label={t("权益（逐日结算）")}
            titled
            volumeLabel={t("成交量")}
            showVolume={false}
            fillArea
            baselineValue={Number.isFinite(deposit) && deposit > 0 ? deposit : undefined}
            priceLines={
              Number.isFinite(deposit) && deposit > 0
                ? [
                    {
                      value: String(record.costs.deposit),
                      label: t("初始资金"),
                      dashed: true,
                      axisLabelVisible: false,
                    },
                  ]
                : undefined
            }
            verticalMarkers={splitMarkers}
            focusedTime={focusedDay}
            onFocusChange={setFocusedDay}
            formatTime={value => day(BigInt(value))}
            describe={index =>
              `${detail.days[index].tradingDay} · ${detail.days[index].equity?.text}`
            }
          />
          <PriceChart
            series={{
              kind: "line",
              points: drawdown.map((value, index) => ({
                timestamp_ns: time(index),
                price: (value * 100).toFixed(2),
              })),
            }}
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
          <MonthlyReturns months={monthlyReturns(record.costs.deposit, detail.days)} />
        </>
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
              {["交易日", "时间", "合约", "方向", "开平", "手数", "价格", "手续费"].map(label => (
                <th scope="col" key={label}>
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
              <td>{groupedText(fill.lots)}</td>
              <td>{fill.price?.text}</td>
              <td>{groupedText(fill.fee)}</td>
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

// Runs side by side: their figures, the parameters that differ, and their net
// value on the first run's days.
export function RunComparison({ ids }: { ids: readonly string[] }) {
  return <Comparison key={ids.join("\n")} joined={ids.join("\n")} />;
}

// `joined` is the run ids, one a line: a parent's new array of the same ids reads nothing again.
function Comparison({ joined }: { joined: string }) {
  const ids = joined.split("\n");
  const [details, setDetails] = useState<RunDetail[]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void Promise.all(joined.split("\n").map(id => lab({ case: "getRun", value: { id } })))
      .then(replies => active && setDetails(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [joined]);
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (details.length !== ids.length) return <p className="subtle">{t("正在读取结果…")}</p>;
  const [first, ...others] = details;
  const name = (detail: RunDetail) => runName(detail.summary!);
  const base = (detail: RunDetail) => Number(JSON.parse(detail.record).costs.deposit);
  const params = [...new Set(details.flatMap(detail => Object.keys(detail.summary!.params)))];
  const differing = params.filter(
    param => new Set(details.map(detail => parameterText(detail.summary!.params[param]))).size > 1,
  );
  return (
    <div className="lab-result">
      <div className="lab-scroll">
        <table className="lab-table lab-compare">
          <thead>
            <tr>
              <th scope="col">
                <span className="sr-only">{t("指标")}</span>
              </th>
              {details.map(detail => (
                <th scope="col" key={detail.summary!.id} className="numeric">
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
      <PriceChart
        series={{
          kind: "line",
          points: first.days.map(item => ({
            timestamp_ns: String(item.settledMs * 1_000_000n),
            price: String(Number(item.equity?.text) / base(first)),
          })),
        }}
        overlays={others.map((detail, index) => {
          const equity = new Map(
            detail.days.map(item => [item.tradingDay, Number(item.equity?.text)]),
          );
          return {
            label: name(detail),
            color: overlayColors[index % overlayColors.length],
            values: first.days.map(item => {
              const value = equity.get(item.tradingDay);
              return value === undefined ? null : value / base(detail);
            }),
          };
        })}
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
    axis === symbolAxis ? t("品种") : axis === batchKey ? t("策略") : axis;
  const grid = sweptAxes.length === 2 ? heatmap(rows, sweptAxes[0]!, sweptAxes[1]!) : null;
  const values = rows.flatMap(row => (row.metric === null ? [] : [row.metric]));
  const low = Math.min(...values),
    high = Math.max(...values);
  const percent = percentMetric(measure);
  const pureMultiSymbol = (hasSymbols && names.length === 0) || names[0] === batchKey;
  const extraMeasure =
    pureMultiSymbol && !["total_return", "sharpe", "max_drawdown"].includes(measure);
  return (
    <div className="lab-result">
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
          <button className="lab-link" onClick={rolling}>
            {t("查看滚动验证的账户")}
          </button>
        )}
      </div>
      {grid && (
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
                  const ratio =
                    cell === null || high === low
                      ? null
                      : lowerBetter
                        ? (high - cell) / (high - low)
                        : (cell - low) / (high - low);
                  return (
                    <td
                      key={c}
                      style={
                        ratio === null
                          ? undefined
                          : {
                              background: `color-mix(in oklab, var(--accent) ${Math.round(12 + 56 * ratio)}%, var(--panel))`,
                            }
                      }
                    >
                      {cell === null ? "—" : metric(cell, percent)}
                    </td>
                  );
                })}
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <VirtualScrollTable
        label={t("排名")}
        columns={
          pureMultiSymbol ? 7 + (extraMeasure ? 1 : 0) : (hasSymbols ? 1 : 0) + names.length + 3
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
            {pureMultiSymbol ? (
              <>
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
              </>
            ) : (
              <th scope="col" className="numeric">
                {metricLabel(measure)}
              </th>
            )}
            <th scope="col">{t("状态")}</th>
          </tr>
        }
        rows={ranked}
        renderRow={(row, index) => (
          <tr key={row.id} className={row.id === selected ? "selected" : undefined}>
            <td>{index + 1}</td>
            {hasSymbols && <td>{row.symbols}</td>}
            {names.map(name => (
              <td key={name}>{row.params[name]}</td>
            ))}
            {pureMultiSymbol && <td className="subtle">{row.span || "—"}</td>}
            {pureMultiSymbol ? (
              <>
                <td className={`numeric ${signed(row.metrics.total_return)}`}>
                  {metric(row.metrics.total_return, true)}
                </td>
                <td className="numeric">{metric(row.metrics.sharpe, false)}</td>
                <td className="numeric">{metric(row.metrics.max_drawdown, true)}</td>
                {extraMeasure && (
                  <td className="numeric">
                    {row.metric === null ? "—" : metric(row.metric, percent)}
                  </td>
                )}
              </>
            ) : (
              <td className="numeric">{row.metric === null ? "—" : metric(row.metric, percent)}</td>
            )}
            <td>
              {row.state === "succeeded" ? (
                <button className="lab-link" onClick={() => open(row.id)}>
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
