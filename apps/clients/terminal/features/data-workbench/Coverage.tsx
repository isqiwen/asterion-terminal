import { useCallback, useEffect, useMemo, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  lab,
  translate,
  type DisplayError,
  type LabCoverage as CoverageReply,
  type LabSummary as Summary,
  type LabTask as Task,
  type LabSource,
  type LabSourceTable,
  type MessageValues,
  type TerminalContext,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states: Record<string, string> = {
  running: "采集中",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
};

function day(offset: number) {
  const value = new Date(Date.now() + offset * 86400000);
  return value.toISOString().slice(0, 10);
}

// The futures warehouse of the Python lab service: what is stored, by product
// and month, and the acquisitions that fill it.
export function Coverage(context: TerminalContext) {
  const [sources, setSources] = useState<LabSource[]>([]);
  const [provider, setProvider] = useState("tushare");
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void lab({ case: "sources", value: {} }, "sources")
      .then(reply => {
        if (active) setSources(reply.sources);
      })
      .catch(failure => {
        if (active) setError(asDisplayError(failure));
      });
    return () => {
      active = false;
    };
  }, []);
  const source = sources.find(item => item.id === provider);
  return (
    <div>
      <ErrorNotice error={error} />
      <label>
        {t("数据源")}
        <select value={provider} onChange={event => setProvider(event.target.value)}>
          {sources.map(item => (
            <option key={item.id} value={item.id}>
              {item.id}
            </option>
          ))}
        </select>
      </label>
      <p>{t("多个来源可同时采集，数据分别保存，不自动切换或混合。")}</p>
      {source && <SourceCoverage key={provider} context={context} source={source} />}
    </div>
  );
}

function SourceCoverage({ context, source }: { context: TerminalContext; source: LabSource }) {
  const provider = source.id;
  const [table, setTable] = useState(source.tables[0]);
  const [exchange, setExchange] = useState(source.exchanges[0]);
  const [contract, setContract] = useState("");
  const [host, setHost] = useState("");
  const [port, setPort] = useState("7450");
  const [preview, setPreview] = useState<LabSourceTable | null>(null);
  const [reading, setReading] = useState(false);
  const [replace, setReplace] = useState(false);
  const [summary, setSummary] = useState<Summary | null>(null);
  const [coverage, setCoverage] = useState<CoverageReply | null>(null);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [begin, setBegin] = useState(() => day(-30));
  const [end, setEnd] = useState(() => day(-1));
  const [submitting, setSubmitting] = useState(false);

  const refresh = useCallback(async () => {
    try {
      const [nextSummary, nextCoverage, nextTasks] = await Promise.all([
        lab({ case: "summary", value: { provider } }, "summary"),
        lab({ case: "coverage", value: { provider } }, "coverage"),
        lab({ case: "tasks", value: {} }, "tasks"),
      ]);
      setSummary(nextSummary);
      setCoverage(nextCoverage);
      setTasks([...nextTasks.tasks].filter(task => task.provider === provider).reverse());
      setError("");
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  }, [provider]);
  const running = tasks.some(task => task.state === "running");
  useEffect(() => {
    void refresh();
  }, [refresh]);
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(() => void refresh(), 3000);
    return () => clearInterval(timer);
  }, [running, refresh]);

  const configure = async () => {
    if (source.requiresCredential) await context.trade("lab.source.configure", {});
    if (source.requiresEndpoint && (host || !source.configured))
      await lab(
        {
          case: "configureSource",
          value: {
            provider,
            host,
            port: Number(port),
          },
        },
        "accepted",
      );
  };
  const readSource = async () => {
    setReading(true);
    setPreview(null);
    try {
      await configure();
      setPreview(
        await lab(
          { case: "sourceRead", value: { provider, table, exchange, day: begin, contract } },
          "sourceTable",
        ),
      );
      setError("");
    } catch (failure) {
      setError(asDisplayError(failure));
    } finally {
      setReading(false);
    }
  };
  const acquire = async () => {
    setSubmitting(true);
    try {
      await configure();
      await lab(
        {
          case: "acquire",
          value: {
            id: `${provider}-${crypto.randomUUID()}`,
            provider,
            table,
            refresh: replace,
            exchanges: [exchange],
            contract: table === "minutes" ? contract : "",
            beginDay: begin,
            endDay: end,
          },
        },
        "accepted",
      );
      await refresh();
    } catch (failure) {
      setError(asDisplayError(failure));
    } finally {
      setSubmitting(false);
    }
  };
  const cancel = async (id: string) => {
    try {
      await lab({ case: "cancel", value: { id } }, "accepted");
      await refresh();
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  };

  const months = useMemo(
    () =>
      [...new Set(coverage?.products.flatMap(item => item.months.map(m => m.month)) ?? [])].sort(),
    [coverage],
  );
  return (
    <div className="data-workbench data-coverage">
      <ErrorNotice error={error} />
      <section aria-label={t("数据集版本")}>
        <h3>{t("数据集版本")}</h3>
        {summary?.datasetVersion ? (
          <>
            <p>
              <code title={summary.datasetVersion}>{summary.datasetVersion.slice(0, 12)}</code>
            </p>
            <table>
              <thead>
                <tr>
                  <th>{t("表")}</th>
                  <th>{t("行数")}</th>
                  <th>{t("分区")}</th>
                  <th>{t("日期范围")}</th>
                </tr>
              </thead>
              <tbody>
                {summary.tables.map(table => (
                  <tr key={table.table}>
                    <td>
                      {t(
                        {
                          daily: "日线",
                          calendar: "交易日历",
                          contracts: "合约表",
                          minutes: "分钟线",
                          settlement: "结算参数",
                          observations: "观测区间",
                        }[table.table] ?? table.table,
                      )}
                    </td>
                    <td>{table.rows.toString()}</td>
                    <td>{table.partitions}</td>
                    <td>{table.firstDay ? `${table.firstDay} – ${table.lastDay}` : "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        ) : (
          <p>{t("仓库里还没有数据。")}</p>
        )}
      </section>
      <section aria-label={t("采集")}>
        <h3>{t("采集")}</h3>
        <label>
          {t("表")}
          <select
            value={table}
            onChange={event => {
              setTable(event.target.value);
              setPreview(null);
            }}
          >
            {source.tables.map(item => (
              <option key={item} value={item}>
                {t(item)}
              </option>
            ))}
          </select>
        </label>
        <label>
          {t("交易所")}
          <select
            value={exchange}
            onChange={event => {
              setExchange(event.target.value);
              setContract("");
              setPreview(null);
            }}
          >
            {source.exchanges.map(item => (
              <option key={item} value={item}>
                {item}
              </option>
            ))}
          </select>
        </label>
        <label>
          {t("合约（查询可选，分钟线必填）")}
          <input
            value={contract}
            placeholder="SHFE/rb/2027-01"
            onChange={event => setContract(event.target.value)}
          />
        </label>
        {source.requiresEndpoint && (
          <>
            <label>
              {t("录制服务地址")}
              <input value={host} onChange={event => setHost(event.target.value)} />
            </label>
            <label>
              {t("端口")}
              <input
                type="number"
                min="1"
                max="65535"
                value={port}
                onChange={event => setPort(event.target.value)}
              />
            </label>
          </>
        )}
        <p>{t("采集按交易所保存，分钟线按合约保存；已入库分区跳过。")}</p>
        <label>
          {t("开始日期")}
          <input type="date" value={begin} onChange={event => setBegin(event.target.value)} />
        </label>
        <label>
          {t("结束日期")}
          <input type="date" value={end} onChange={event => setEnd(event.target.value)} />
        </label>
        <label>
          <input
            type="checkbox"
            checked={replace}
            onChange={event => setReplace(event.target.checked)}
          />
          {t("重新采集已有分区，保留旧数据集版本")}
        </label>
        <button
          type="button"
          disabled={
            submitting ||
            running ||
            !begin ||
            !end ||
            !["daily", "minutes", "settlement"].includes(table) ||
            (table === "minutes" && !contract)
          }
          onClick={acquire}
        >
          {t("开始采集")}
        </button>
        <button
          type="button"
          disabled={reading || !begin || (table === "minutes" && !contract)}
          onClick={() => void readSource()}
        >
          {t("查询开始日期的数据")}
        </button>
        {preview && (
          <section aria-label={t("来源查询结果")}>
            <p>
              {preview.provider} · {t(preview.table)} ·{" "}
              {t("共 {count} 行，预览前 50 行", { count: preview.rows.length })}
            </p>
            <div className="coverage-matrix">
              <table>
                <thead>
                  <tr>
                    {preview.columns.map(column => (
                      <th key={column}>{column}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {preview.rows.slice(0, 50).map((row, index) => (
                    <tr key={index}>
                      {row.cells.map((cell, i) => (
                        <td key={i}>{cell.value ?? "—"}</td>
                      ))}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            {preview.observations.length > 0 && (
              <>
                <p>
                  {t("以下是录制服务的观测区间，不代表此合约全时段完整；区间之间可能存在缺口。")}
                </p>
                <ul>
                  {preview.observations.map(item => (
                    <li key={`${item.instance}/${item.number}`}>
                      {new Date(Number(item.firstReceivedMs)).toISOString()} –{" "}
                      {new Date(Number(item.lastReceivedMs)).toISOString()} · {item.end}
                    </li>
                  ))}
                </ul>
              </>
            )}
          </section>
        )}
        {tasks.length > 0 && (
          <table aria-label={t("采集任务")}>
            <thead>
              <tr>
                <th>{t("表")}</th>
                <th>{t("区间")}</th>
                <th>{t("状态")}</th>
                <th>{t("进度")}</th>
                <th>{t("请求次数")}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {tasks.map(task => (
                <tr key={task.id}>
                  <td>{t(task.table)}</td>
                  <td>
                    {task.beginDay} – {task.endDay}
                  </td>
                  <td title={task.error}>
                    {t(states[task.state] ?? task.state)}
                    {task.error ? ` · ${task.error}` : ""}
                  </td>
                  <td>
                    {task.daysDone} / {task.daysTotal}
                    {task.daysEmpty
                      ? ` · ${t("{count} 天来源尚无数据", { count: task.daysEmpty })}`
                      : ""}
                  </td>
                  <td>{task.requests}</td>
                  <td>
                    {task.state === "running" && (
                      <button type="button" onClick={() => void cancel(task.id)}>
                        {t("取消")}
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
      <section aria-label={t("日线覆盖")}>
        <h3>{t("日线覆盖")}</h3>
        <p>{t("每格为该来源已存日线的天数；没有交易日历时，完整性未知。")}</p>
        {coverage && coverage.products.length > 0 ? (
          <div className="coverage-matrix">
            <table>
              <thead>
                <tr>
                  <th>{t("品种")}</th>
                  {months.map(month => (
                    <th key={month}>{month}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {coverage.products.map(product => {
                  const byMonth = new Map(product.months.map(m => [m.month, m]));
                  return (
                    <tr key={`${product.exchange}/${product.product}`}>
                      <th scope="row">
                        {product.exchange}/{product.product}
                      </th>
                      {months.map(month => {
                        const cell = byMonth.get(month);
                        const share = cell && cell.openDays ? cell.days / cell.openDays : 0;
                        return (
                          <td
                            key={month}
                            data-share={cell ? (share >= 1 ? "full" : "partial") : "none"}
                            title={cell ? `${cell.days} / ${cell.openDays || "?"}` : t("无数据")}
                          >
                            {cell ? cell.days : ""}
                          </td>
                        );
                      })}
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        ) : (
          <p>{t("仓库里还没有日线。")}</p>
        )}
      </section>
    </div>
  );
}
