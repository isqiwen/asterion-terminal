import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { RecordingStatus } from "./RecordingStatus";
import {
  ErrorNotice,
  asDisplayError,
  marketData,
  translate,
  type DisplayError,
  type MarketCoverage,
  type MarketSummary,
  type MarketTask,
  type MarketSource,
  type MessageValues,
  type TerminalContext,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states: Record<string, string> = {
  queued: "等待采集",
  running: "采集中",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
const activeTask = (task: MarketTask) => ["queued", "running"].includes(task.state);
const day = (offset: number) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
const tableName = (name: string) =>
  t(
    {
      daily: "日线",
      calendar: "交易日历",
      contracts: "合约表",
      minutes: "分钟线",
      settlement: "结算参数",
      observations: "观测区间",
    }[name] ?? name,
  );
type Source = Pick<MarketSource, "provider" | "tables" | "exchanges">;

// Every read and acquisition goes to the saved remote Market. Browsing a
// source changes only this component, never the service's collection lifecycle.
export function Coverage(context: TerminalContext) {
  const endpoint = context.snapshot?.market_endpoint;
  const selection =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  return <DataPage key={selection} context={context} />;
}

function DataPage({ context }: { context: TerminalContext }) {
  const [sources, setSources] = useState<Source[]>([]);
  const [provider, setProvider] = useState("");
  const [canManage, setCanManage] = useState<boolean | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [retry, setRetry] = useState(0);
  useEffect(() => {
    let active = true;
    const load = async () => {
      try {
        const catalog = await marketData(
          { case: "acquisitionSources", value: {} },
          "acquisitionSources",
        );
        const listed = new Map<string, Source>(catalog.sources.map(item => [item.provider, item]));
        let afterProvider = "";
        do {
          const page = await marketData({ case: "datasets", value: { afterProvider } }, "datasets");
          for (const item of page.sources) {
            // A disabled provider's already stored dataset is still readable.
            if (!listed.has(item.provider))
              listed.set(item.provider, { provider: item.provider, tables: [], exchanges: [] });
          }
          afterProvider = page.nextProvider;
        } while (afterProvider && active);
        if (!active) return;
        const next = [...listed.values()].sort((a, b) => a.provider.localeCompare(b.provider));
        setSources(next);
        setProvider(current =>
          next.some(item => item.provider === current) ? current : (next[0]?.provider ?? ""),
        );
        setCanManage(catalog.canManage);
        setError("");
      } catch (failure) {
        if (active) setError(asDisplayError(failure));
      }
    };
    void load();
    return () => {
      active = false;
    };
  }, [retry]);
  const source = sources.find(item => item.provider === provider);
  return (
    <div>
      <RecordingStatus context={context} />
      <ErrorNotice error={error} />
      <button onClick={() => setRetry(value => value + 1)}>{t("刷新来源")}</button>
      <label>
        {t("数据源")}
        <select value={provider} onChange={event => setProvider(event.target.value)}>
          {sources.map(item => (
            <option key={item.provider} value={item.provider}>
              {item.provider === "recorder" ? t("Market 录制") : item.provider}
            </option>
          ))}
        </select>
      </label>
      <p>{t("多个来源可同时采集，数据分别保存，不自动切换或混合。")}</p>
      <p>{t("来源配置和凭据由 Market 主机管理；关闭此页面不会停止采集。")}</p>
      {canManage === false && (
        <p role="status">{t("当前连接只有读取权限，不能启动或取消采集。")}</p>
      )}
      {!source && !error && canManage !== null && (
        <p>{t("Market 尚未启用采集来源，也没有已保存的数据集。")}</p>
      )}
      {source && (
        <SourceCoverage
          key={`${provider}/${source.tables.join()}/${source.exchanges.join()}`}
          source={source}
          canManage={canManage === true}
        />
      )}
    </div>
  );
}

function SourceCoverage({ source, canManage }: { source: Source; canManage: boolean }) {
  const provider = source.provider;
  const [table, setTable] = useState(source.tables[0] ?? "");
  const [exchange, setExchange] = useState(source.exchanges[0] ?? "");
  const [contract, setContract] = useState("");
  const [replace, setReplace] = useState(false);
  const [summary, setSummary] = useState<MarketSummary | null>(null);
  const [coverage, setCoverage] = useState<MarketCoverage["products"]>([]);
  const [tasks, setTasks] = useState<MarketTask[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [actionError, setActionError] = useState<DisplayError>("");
  const [begin, setBegin] = useState(() => day(-30));
  const [end, setEnd] = useState(() => day(-1));
  const [submitting, setSubmitting] = useState(false);
  const [submitted, setSubmitted] = useState("");
  const mounted = useRef(false);
  const pending = useRef(false);

  const refresh = useCallback(async () => {
    if (pending.current) return;
    pending.current = true;
    try {
      const nextSummary = await marketData(
        { case: "datasetSummary", value: { provider } },
        "datasetSummary",
      );
      const products: MarketCoverage["products"] = [];
      if (nextSummary.datasetVersion) {
        let afterProduct = "";
        do {
          const page = await marketData(
            {
              case: "datasetCoverage",
              value: {
                provider,
                datasetVersion: nextSummary.datasetVersion,
                afterProduct,
              },
            },
            "datasetCoverage",
          );
          products.push(...page.products);
          afterProduct = page.nextProduct;
        } while (afterProduct && mounted.current);
      }
      const nextTasks: MarketTask[] = [];
      let afterId = "";
      do {
        const page = await marketData(
          { case: "acquisitionTasks", value: { afterId } },
          "acquisitionTasks",
        );
        nextTasks.push(...page.tasks.filter(task => task.request?.provider === provider));
        afterId = page.nextId;
      } while (afterId && mounted.current);
      if (!mounted.current) return;
      setSummary(nextSummary);
      setCoverage(products);
      setTasks(
        nextTasks.sort((a, b) =>
          a.startedNs > b.startedNs ? -1 : a.startedNs < b.startedNs ? 1 : 0,
        ),
      );
      setError("");
    } catch (failure) {
      if (mounted.current) setError(asDisplayError(failure));
    } finally {
      pending.current = false;
    }
  }, [provider]);
  useEffect(() => {
    mounted.current = true;
    void refresh();
    // Other clients can submit work at any time. Poll even with no local task.
    const timer = setInterval(() => {
      if (!document.hidden) void refresh();
    }, 3000);
    return () => {
      mounted.current = false;
      clearInterval(timer);
    };
  }, [refresh]);
  const running = tasks.some(activeTask);
  const acquire = async () => {
    setSubmitting(true);
    setActionError("");
    const id = `${provider}-${crypto.randomUUID()}`;
    setSubmitted(id);
    try {
      await marketData(
        {
          case: "acquisitionStart",
          value: {
            id,
            provider,
            table,
            refresh: replace,
            exchanges: [exchange],
            contract: table === "minutes" ? contract : "",
            beginDay: begin,
            endDay: end,
          },
        },
        "acquisitionTask",
      );
      await refresh();
    } catch (failure) {
      if (mounted.current) setActionError(asDisplayError(failure));
    } finally {
      if (mounted.current) setSubmitting(false);
    }
  };
  const cancel = async (id: string) => {
    setActionError("");
    try {
      await marketData({ case: "acquisitionCancel", value: { id } }, "acquisitionTask");
      await refresh();
    } catch (failure) {
      if (mounted.current) setActionError(asDisplayError(failure));
    }
  };
  const months = useMemo(
    () => [...new Set(coverage.flatMap(item => item.months.map(m => m.month)))].sort(),
    [coverage],
  );
  return (
    <div className="data-workbench data-coverage">
      <ErrorNotice error={error} />
      <ErrorNotice error={actionError} />
      <button onClick={() => void refresh()}>{t("刷新数据与任务")}</button>
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
                {summary.tables.map(item => (
                  <tr key={item.table}>
                    <td>{tableName(item.table)}</td>
                    <td>{item.rows.toString()}</td>
                    <td>{item.partitions.toString()}</td>
                    <td>{item.firstDay ? `${item.firstDay} – ${item.lastDay}` : "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        ) : (
          <p>{t("仓库里还没有数据。")}</p>
        )}
      </section>
      {source.tables.length > 0 && (
        <section aria-label={t(source.provider === "recorder" ? "发布录制分钟" : "采集")}>
          <h3>{t(source.provider === "recorder" ? "发布录制分钟" : "采集")}</h3>
          <label>
            {t("表")}
            <select value={table} onChange={event => setTable(event.target.value)}>
              {source.tables.map(item => (
                <option key={item} value={item}>
                  {tableName(item)}
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
              }}
            >
              {source.exchanges.map(item => (
                <option key={item}>{item}</option>
              ))}
            </select>
          </label>
          {table === "minutes" && (
            <label>
              {t("合约（分钟线必填）")}
              <input
                value={contract}
                placeholder="SHFE/rb/2027-01"
                onChange={event => setContract(event.target.value)}
              />
            </label>
          )}
          <p>
            {t(
              source.provider === "recorder"
                ? "将 Market 已落盘的分钟发布为研究版本，保留不完整分钟和观测中断；补录后可重新发布，旧版本仍可读取。"
                : "采集按交易所保存，分钟线按合约保存；已入库分区跳过。",
            )}
          </p>
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
            {t(
              source.provider === "recorder"
                ? "重新发布已有分区，保留旧数据集版本"
                : "重新采集已有分区，保留旧数据集版本",
            )}
          </label>
          <button
            type="button"
            disabled={
              !canManage ||
              submitting ||
              running ||
              !begin ||
              !end ||
              begin > end ||
              !exchange ||
              !table ||
              (table === "minutes" && !contract)
            }
            onClick={() => void acquire()}
          >
            {t(source.provider === "recorder" ? "发布录制分钟" : "开始采集")}
          </button>
          {submitted && (
            <p>
              {t("本次提交任务")}: <code>{submitted}</code>
            </p>
          )}
        </section>
      )}
      {tasks.length > 0 && (
        <table aria-label={t("采集任务")}>
          <thead>
            <tr>
              <th>{t("任务编号")}</th>
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
              <tr key={task.request!.id}>
                <td>
                  <code>{task.request!.id}</code>
                </td>
                <td>{tableName(task.request!.table)}</td>
                <td>
                  {task.request!.beginDay} – {task.request!.endDay}
                </td>
                <td title={task.error}>
                  {t(states[task.state] ?? task.state)}
                  {task.error ? ` · ${task.error}` : ""}
                </td>
                <td>
                  {task.daysDone.toString()} / {task.daysTotal.toString()}
                  {task.daysEmpty > 0n
                    ? ` · ${t("{count} 天来源尚无数据", { count: Number(task.daysEmpty) })}`
                    : ""}
                </td>
                <td>{task.requests.toString()}</td>
                <td>
                  {canManage && activeTask(task) && (
                    <button onClick={() => void cancel(task.request!.id)}>{t("取消")}</button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <section aria-label={t("日线覆盖")}>
        <h3>{t("日线覆盖")}</h3>
        <p>{t("每格为该来源已存日线的天数；没有交易日历时，完整性未知。")}</p>
        {coverage.length ? (
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
                {coverage.map(product => {
                  const byMonth = new Map(product.months.map(m => [m.month, m]));
                  return (
                    <tr key={`${product.exchange}/${product.product}`}>
                      <th scope="row">
                        {product.exchange}/{product.product}
                      </th>
                      {months.map(month => {
                        const cell = byMonth.get(month);
                        const share = cell?.openDays ? cell.days / cell.openDays : 0;
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
