import { useEffect, useMemo, useState } from "react";
import { PriceChart, type Candle, type TradeMark } from "@asterion/terminal/charts/PriceChart";
import {
  ErrorNotice,
  asDisplayError,
  lab,
  protocolPeriod,
  translate,
  type DisplayError,
  type LabChartBars,
  type LabRunDetail,
  type LabRunReport,
  type LabRunSummary,
  type MessageValues,
} from "../contract";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import { heatmap, monthlyReturns, parameterText, runState } from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The figures every run has, in the order the result shows them.
const figures = [
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

function Figures({ summary, prefix = "" }: { summary: LabRunSummary; prefix?: string }) {
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
  const [detail, setDetail] = useState<LabRunDetail | null>(null);
  const [contract, setContract] = useState("");
  const [bars, setBars] = useState<LabChartBars["bars"][number][]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    setDetail(null);
    setError("");
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
  useEffect(() => {
    let active = true;
    setBars([]);
    if (!contract || !record) return;
    // The bars the run read: days, or minutes of its period.
    const bars = record.data.bars;
    const minutes = bars.period === "day" ? 0 : Number(String(bars.period).replace("m", ""));
    const datasetVersion = record.data.dataset_version;
    if (!datasetVersion) {
      setError(asDisplayError(new Error(t("运行记录缺少数据版本，无法读取对应 K 线。"))));
      return;
    }
    void lab({
      case: "getChartBars",
      value: { contract, period: protocolPeriod(minutes || "day"), datasetVersion },
    })
      .then(reply => {
        if (!active) return;
        if (reply.datasetVersion !== datasetVersion)
          throw new Error(t("返回的 K 线与运行使用的数据版本不一致。"));
        // Only the run's range of a contract's stored bars.
        const first = record.data.begin_day as string,
          last = record.data.end_day as string;
        setBars(reply.bars.filter(bar => bar.tradingDay >= first && bar.tradingDay <= last));
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [contract, record]);
  if (error) return <ErrorNotice error={error} />;
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
  const contracts = [...new Set(detail.fills.map(fill => fill.contract))];
  const daily = record.data.bars.period === "day";
  const candles: Candle[] = bars.map(bar => ({
    timestamp_ns: String(bar.endMs * 1_000_000n),
    open: bar.open?.text ?? "",
    high: bar.high?.text ?? "",
    low: bar.low?.text ?? "",
    close: bar.close?.text ?? "",
    volume: bar.volume?.text ?? "0",
  }));
  const trades: TradeMark[] = detail.fills
    .filter(fill => fill.contract === contract)
    .map(fill => ({
      // A day's fills sit on its bar; a minute fill on the bar that ends at its time.
      index: bars.findIndex(bar =>
        daily ? bar.tradingDay === fill.tradingDay : bar.endMs === fill.timeMs,
      ),
      side: fill.side === Side.BUY ? ("buy" as const) : ("sell" as const),
      text: `${fill.side === Side.BUY ? t("买") : t("卖")} ${Number(fill.lots?.text)}@${Number(fill.price?.text)}`,
    }));
  const months = monthlyReturns(record.costs.deposit, detail.days);
  const holdout = record.statistics?.holdout_day as string | undefined;
  const rolling = record.statistics?.rolling;
  const excluded = record.data.excluded as { bars: number; days: string[] } | undefined;
  const rejections = (record.data.rejections ?? []) as Rejection[];
  const refused = rejections.reduce((sum, row) => sum + row.count, 0);
  return (
    <div className="lab-result">
      {excluded && excluded.bars > 0 && (
        <p className="subtle">
          {t("{bars} 根 K 线含不完整的分钟，未参与回测：{days}", {
            bars: excluded.bars,
            days: excluded.days.join("、"),
          })}
        </p>
      )}
      {refused > 0 && (
        <details className="lab-rejections">
          <summary>
            {t("目标被风控或资金拒绝 {count} 次，实盘遇到会结束实例", { count: refused })}
          </summary>
          <div className="lab-scroll">
            <table className="lab-table">
              <thead>
                <tr>
                  {["合约", "目标手数", "原因", "次数", "首次", "末次"].map(label => (
                    <th key={label}>{t(label)}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rejections.map(row => (
                  <tr key={`${row.contract} ${row.lots} ${row.reason}`}>
                    <td>{row.contract}</td>
                    <td>{row.lots}</td>
                    <td>{t(reasons[row.reason] ?? row.reason)}</td>
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
        <>
          <h5>{t("留出日 {day} 之前", { day: holdout })}</h5>
          <Figures summary={summary} prefix="development_" />
          <h5>{t("留出段")}</h5>
          <Figures summary={summary} prefix="holdout_" />
        </>
      )}
      {rolling && (
        <>
          <h5>{t("样本外（第一轮验证起）")}</h5>
          <Figures summary={summary} prefix="out_of_sample_" />
        </>
      )}
      <PriceChart
        series={{
          kind: "line",
          points: detail.days.map(item => ({
            timestamp_ns: String(item.settledMs * 1_000_000n),
            price: item.equity?.text ?? "",
          })),
        }}
        label={t("权益（逐日结算）")}
        volumeLabel={t("成交量")}
        showVolume={false}
        fillArea
        formatTime={value => day(BigInt(value))}
        describe={index => `${detail.days[index].tradingDay} · ${detail.days[index].equity?.text}`}
      />
      {contracts.length > 0 && (
        <>
          <label className="lab-inline">
            {t("K 线与买卖点")}
            <select value={contract} onChange={event => setContract(event.target.value)}>
              {contracts.map(item => (
                <option key={item}>{item}</option>
              ))}
            </select>
          </label>
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
      )}
      <h5>{t("月度收益")}</h5>
      <div className="lab-months">
        {months.map(item => (
          <span key={item.month} className={signed(item.value)}>
            <small>{item.month}</small>
            {metric(item.value, true)}
          </span>
        ))}
      </div>
      <details>
        <summary>{t("成交（{count} 笔）", { count: detail.fills.length })}</summary>
        <div className="lab-scroll">
          <table className="lab-table">
            <thead>
              <tr>
                {["交易日", "时间", "合约", "方向", "开平", "手数", "价格", "手续费"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {detail.fills.map(fill => (
                <tr key={fill.id}>
                  <td>{fill.tradingDay}</td>
                  <td>{minute(fill.timeMs * 1_000_000n)}</td>
                  <td>{fill.contract}</td>
                  <td>{fill.side === Side.BUY ? t("买") : t("卖")}</td>
                  <td>{t(offsets[Offset[fill.offset].toLowerCase()] ?? Offset[fill.offset])}</td>
                  <td>{fill.lots?.text}</td>
                  <td>{fill.price?.text}</td>
                  <td>{fill.fee?.text}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </details>
      <QuantStats id={id} />
      <RunDetails record={detail.record} openVersion={openVersion} />
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

// QuantStats figures of the run, computed by the lab when first opened.
function QuantStats({ id }: { id: string }) {
  const [report, setReport] = useState<LabRunReport | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [asked, setAsked] = useState(false);
  useEffect(() => {
    if (!asked) return;
    let active = true;
    void lab({ case: "getRunReport", value: { id } })
      .then(reply => active && setReport(reply))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [asked, id]);
  return (
    <details onToggle={event => event.currentTarget.open && setAsked(true)}>
      <summary>{t("QuantStats 指标")}</summary>
      {error ? (
        <ErrorNotice error={error} />
      ) : !report ? (
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
            {t("按逐日结算权益的日收益计算，无风险利率为零，一年 252 期。")}
          </p>
        </>
      )}
    </details>
  );
}

const offsets: Record<string, string> = {
  open: "开仓",
  close_today: "平今",
  close_yesterday: "平昨",
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
  interrupted: "已中断",
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
    <details className="lab-details">
      <summary>{t("复现信息")}</summary>
      <dl>
        <dt>{t("策略")}</dt>
        <dd>
          {value.strategy.id} @ <code>{value.strategy.commit.slice(0, 12)}</code>
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
          · {value.data.symbols.join(", ")} · {value.data.begin_day} → {value.data.end_day}
        </dd>
        <dt>{t("引擎")}</dt>
        <dd>{value.engine.version}</dd>
      </dl>
      <pre>
        {JSON.stringify({ params: value.params, costs: value.costs, risk: value.risk }, null, 2)}
      </pre>
    </details>
  );
}

// Two runs side by side: their figures, and their equity on the days both have.
export function RunComparison({ ids }: { ids: readonly [string, string] }) {
  return <Comparison key={ids.join(":")} ids={ids} />;
}

function Comparison({ ids: [firstId, secondId] }: { ids: readonly [string, string] }) {
  const [details, setDetails] = useState<LabRunDetail[]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void Promise.all([firstId, secondId].map(id => lab({ case: "getRun", value: { id } })))
      .then(replies => active && setDetails(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [firstId, secondId]);
  if (error) return <ErrorNotice error={error} />;
  if (details.length !== 2) return <p className="subtle">{t("正在读取结果…")}</p>;
  const [first, second] = details;
  const other = new Map(second.days.map(item => [item.tradingDay, Number(item.equity?.text)]));
  const base = (detail: LabRunDetail) => Number(JSON.parse(detail.record).costs.deposit);
  return (
    <div className="lab-result">
      <div className="lab-scroll">
        <table className="lab-table">
          <thead>
            <tr>
              <th />
              {details.map(detail => (
                <th key={detail.summary!.id}>{detail.summary!.id}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {figures.map(([name, label, percent]) => (
              <tr key={name}>
                <td>{t(label)}</td>
                {details.map(detail => (
                  <td key={detail.summary!.id}>{metric(detail.summary!.metrics[name], percent)}</td>
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
        overlays={[
          {
            label: second.summary!.id,
            color: "#e0a030",
            values: first.days.map(item => {
              const value = other.get(item.tradingDay);
              return value === undefined ? null : value / base(second);
            }),
          },
        ]}
        label={t("净值：{first} 与 {second}", {
          first: first.summary!.id,
          second: second.summary!.id,
        })}
        volumeLabel={t("成交量")}
        showVolume={false}
        formatTime={value => day(BigInt(value))}
        describe={index => first.days[index].tradingDay}
      />
    </div>
  );
}

// A sweep's runs ranked by a metric, and with two swept parameters a grid of it.
export function SweepResult({
  runs,
  selected,
  open,
  rolling,
}: {
  runs: readonly LabRunSummary[];
  selected: string;
  open: (id: string) => void;
  rolling?: () => void;
}) {
  const names = Object.keys(runs[0]?.params ?? {});
  const [measure, setMeasure] = useState<string>(
    runs.some(run => "development_sharpe" in run.metrics) ? "development_sharpe" : "sharpe",
  );
  const rows = runs.map(run => ({
    id: run.id,
    state: runState(run.state),
    params: Object.fromEntries(names.map(name => [name, parameterText(run.params[name])])),
    metric: run.metrics[measure] ?? null,
  }));
  const ranked = [...rows].sort((a, b) => (b.metric ?? -Infinity) - (a.metric ?? -Infinity));
  const grid = names.length === 2 ? heatmap(rows, names[0], names[1]) : null;
  const values = rows.flatMap(row => (row.metric === null ? [] : [row.metric]));
  const low = Math.min(...values),
    high = Math.max(...values);
  const percent = percentMetric(measure);
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
              <th>
                {names[0]} \ {names[1]}
              </th>
              {grid.columns.map(column => (
                <th key={column}>{column}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {grid.rows.map((row, r) => (
              <tr key={row}>
                <th>{row}</th>
                {grid.cells[r].map((cell, c) => (
                  <td
                    key={c}
                    style={
                      cell === null || high === low
                        ? undefined
                        : {
                            background: `rgba(82, 120, 206, ${(0.15 + (0.7 * (cell - low)) / (high - low)).toFixed(2)})`,
                          }
                    }
                  >
                    {cell === null ? "—" : metric(cell, percent)}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <div className="lab-scroll">
        <table className="lab-table">
          <thead>
            <tr>
              <th>{t("排名")}</th>
              {names.map(name => (
                <th key={name}>{name}</th>
              ))}
              <th className="numeric">{metricLabel(measure)}</th>
              <th>{t("状态")}</th>
            </tr>
          </thead>
          <tbody>
            {ranked.map((row, index) => (
              <tr key={row.id} className={row.id === selected ? "selected" : undefined}>
                <td>{index + 1}</td>
                {names.map(name => (
                  <td key={name}>{row.params[name]}</td>
                ))}
                <td className="numeric">
                  {row.metric === null ? "—" : metric(row.metric, percent)}
                </td>
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
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
