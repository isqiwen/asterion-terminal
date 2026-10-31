import { useEffect, useMemo, useState } from "react";
import { PriceChart, type Candle, type TradeMark } from "@asterion/client-ui/PriceChart";
import {
  ErrorNotice,
  asDisplayError,
  lab,
  translate,
  type DisplayError,
  type LabDailyBar,
  type LabRunDetail,
  type LabRunSummary,
  type MessageValues,
} from "../contract";
import { heatmap, monthlyReturns, parameterText } from "./research-lab";

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
export function RunResult({ id, state }: { id: string; state: string }) {
  const [detail, setDetail] = useState<LabRunDetail | null>(null);
  const [contract, setContract] = useState("");
  const [bars, setBars] = useState<LabDailyBar[]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    setDetail(null);
    setError("");
    void lab({ case: "run", value: { id } }, "run")
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
    void lab(
      {
        case: "dailyBars",
        value: {
          contract,
          provider: record.data.provider,
          beginDay: record.data.begin_day,
          endDay: record.data.end_day,
        },
      },
      "dailyBars",
    )
      .then(reply => active && setBars(reply.bars))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [contract, record]);
  if (error) return <ErrorNotice error={error} />;
  if (!detail || !record) return <p className="subtle">{t("正在读取结果…")}</p>;
  const summary = detail.summary!;
  if (summary.state !== "succeeded")
    return (
      <div className="lab-result">
        <p role={summary.state === "failed" ? "alert" : undefined}>
          {t(states[summary.state] ?? "未开始")}
          {summary.error ? ` · ${summary.error}` : ""}
        </p>
        <RunDetails record={detail.record} />
      </div>
    );
  const contracts = [...new Set(detail.fills.map(fill => fill.contract))];
  const candles: Candle[] = bars
    .filter(bar => bar.open && bar.close)
    .map(bar => ({
      timestamp_ns: String(BigInt(Date.parse(`${bar.day}T07:00:00Z`)) * 1_000_000n),
      open: bar.open,
      high: bar.high,
      low: bar.low,
      close: bar.close,
      volume: bar.volume || "0",
    }));
  const trades: TradeMark[] = detail.fills
    .filter(fill => fill.contract === contract)
    .map(fill => ({
      index: bars.findIndex(bar => bar.day === day(fill.timeNs)),
      side: fill.side === "buy" ? ("buy" as const) : ("sell" as const),
      text: `${fill.side === "buy" ? t("买") : t("卖")} ${Number(fill.quantity)}@${Number(fill.price)}`,
    }));
  const months = monthlyReturns(record.costs.deposit, detail.days);
  const holdout = record.statistics?.holdout_day as string | undefined;
  const rolling = record.statistics?.rolling;
  return (
    <div className="lab-result">
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
            timestamp_ns: String(item.timeNs),
            price: item.equity,
          })),
        }}
        label={t("权益（逐日结算）")}
        volumeLabel={t("成交量")}
        showVolume={false}
        fillArea
        formatTime={value => day(BigInt(value))}
        describe={index => `${detail.days[index].tradingDay} · ${detail.days[index].equity}`}
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
              formatTime={value => day(BigInt(value))}
              describe={index =>
                `${bars[index].day} · ${bars[index].open} ${bars[index].high} ${bars[index].low} ${bars[index].close}`
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
                {["交易日", "合约", "方向", "开平", "手数", "价格", "手续费"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {detail.fills.map(fill => (
                <tr key={fill.fill}>
                  <td>{day(fill.timeNs)}</td>
                  <td>{fill.contract}</td>
                  <td>{fill.side === "buy" ? t("买") : t("卖")}</td>
                  <td>{t(offsets[fill.offset] ?? fill.offset)}</td>
                  <td>{fill.quantity}</td>
                  <td>{fill.price}</td>
                  <td>{fill.fee}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </details>
      <RunDetails record={detail.record} />
    </div>
  );
}

const offsets: Record<string, string> = {
  open: "开仓",
  close_today: "平今",
  close_yesterday: "平昨",
};
export const states: Record<string, string> = {
  running: "运行中",
  succeeded: "已完成",
  failed: "失败",
  interrupted: "已中断",
};

// What reproduces the run: strategy version, data, engine, costs and limits.
function RunDetails({ record }: { record: string }) {
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
          {value.data.provider} · <code>{value.data.dataset_version.slice(0, 12)}</code> ·{" "}
          {value.data.symbols.join(", ")} · {value.data.begin_day} → {value.data.end_day}
        </dd>
        <dt>{t("引擎")}</dt>
        <dd>
          {value.engine.version} · <code>{value.engine.requirements.slice(0, 12)}</code>
        </dd>
      </dl>
      <pre>
        {JSON.stringify({ params: value.params, costs: value.costs, risk: value.risk }, null, 2)}
      </pre>
    </details>
  );
}

// Two runs side by side: their figures, and their equity on the days both have.
export function RunComparison({ ids }: { ids: readonly [string, string] }) {
  const [details, setDetails] = useState<LabRunDetail[]>([]);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void Promise.all(ids.map(id => lab({ case: "run", value: { id } }, "run")))
      .then(replies => active && setDetails(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [ids]);
  if (error) return <ErrorNotice error={error} />;
  if (details.length !== 2) return <p className="subtle">{t("正在读取结果…")}</p>;
  const [first, second] = details;
  const other = new Map(second.days.map(item => [item.tradingDay, Number(item.equity)]));
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
            timestamp_ns: String(item.timeNs),
            price: String(Number(item.equity) / base(first)),
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
    state: run.state,
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
