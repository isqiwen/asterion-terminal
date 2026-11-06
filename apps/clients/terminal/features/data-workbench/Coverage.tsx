import { Table, TaskState } from "../../src/generated/asterion/market_pb";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { MinuteComparison } from "./MinuteComparison";
import { ContractPicker } from "./ContractPicker";
import { DatasetHistory, changeText } from "./DatasetHistory";
import { MinuteCoverage } from "./MinuteCoverage";
import { useMarket } from "../../src/host/market";
import { RecordedDays } from "./RecordedDays";
import { RecordingStatus } from "./RecordingStatus";
import { ReferenceTables, referenceTables } from "./ReferenceTables";
import { PriceTickImport } from "./PriceTickImport";
import {
  ErrorNotice,
  asDisplayError,
  marketData,
  translate,
  type DisplayError,
  type MarketChange,
  type MarketCoverage,
  type MarketSummary,
  type MarketTask,
  type MarketSource,
  type MessageValues,
  type TerminalContext,
  ActionError,
  MarketUnset,
  useActionError,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states: Record<number, string> = {
  [TaskState.QUEUED]: "等待采集",
  [TaskState.RUNNING]: "采集中",
  [TaskState.SUCCEEDED]: "已完成",
  [TaskState.FAILED]: "失败",
  [TaskState.CANCELLED]: "已取消",
  [TaskState.INTERRUPTED]: "已中断",
};
const activeTask = (task: MarketTask) => [TaskState.QUEUED, TaskState.RUNNING].includes(task.state);
const day = (offset: number) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
// A month's days up to yesterday: providers only hold finished trading days.
const monthRange = (month: string) => {
  const [year, number] = month.split("-").map(Number);
  const last = new Date(Date.UTC(year!, number!, 0)).toISOString().slice(0, 10);
  return [`${month}-01`, last < day(-1) ? last : day(-1)] as const;
};
const tables: Partial<Record<Table, string>> = {
  [Table.DAILY]: "日线",
  [Table.CALENDAR]: "交易日历",
  [Table.CONTRACTS]: "合约表",
  [Table.MINUTES]: "分钟线",
  [Table.SETTLEMENT]: "结算参数",
  [Table.OBSERVATIONS]: "观测区间",
  [Table.PRICE_TICKS]: "历史报价单位",
};
const tableName = (table: Table) => t(tables[table] ?? Table[table]);
// Sources such as research list no exchanges of their own.
const exchanges = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];
type Source = Pick<MarketSource, "provider" | "tables" | "exchanges">;

// Every read and acquisition goes to the saved remote Market. Browsing a
// source changes only this component, never the service's collection lifecycle.
export function Coverage(context: TerminalContext) {
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  return (
    <section className="data-page" aria-label={t("行情数据")}>
      <div className="panel-heading">
        <h2>{t("行情数据")}</h2>
        {endpoint && (
          <span className="subtle">
            {endpoint.host}:{endpoint.port}
          </span>
        )}
      </div>
      {endpoint ? (
        <DataPage
          key={`${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`}
          context={context}
        />
      ) : (
        <MarketUnset openSettings={context.openSettings} />
      )}
    </section>
  );
}

// Another page can open a source's version: params `provider` and `version`.
function DataPage({ context }: { context: TerminalContext }) {
  const opened = context.workspaceParams;
  const [sources, setSources] = useState<Source[]>([]);
  const [provider, setProvider] = useState(opened?.provider ?? "");
  const [imported, setImported] = useState<{ provider: string; version: string }>();
  useEffect(() => {
    if (opened?.provider) setProvider(opened.provider);
    setImported(undefined);
  }, [opened?.provider, opened?.version]);
  const [canManage, setCanManage] = useState<boolean | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [retry, setRetry] = useState(0);
  useEffect(() => {
    let active = true;
    const load = async () => {
      try {
        const catalog = await marketData({ case: "listSources", value: {} });
        const listed = new Map<string, Source>(catalog.sources.map(item => [item.provider, item]));
        const stored = await marketData({ case: "listDatasets", value: {} });
        for (const item of stored.datasets) {
          // A disabled provider's already stored dataset is still readable.
          if (!listed.has(item.provider))
            listed.set(item.provider, { provider: item.provider, tables: [], exchanges: [] });
        }
        if (!active) return;
        // What research reads comes first.
        const next = [...listed.values()].sort(
          (a, b) =>
            Number(b.provider === "research") - Number(a.provider === "research") ||
            a.provider.localeCompare(b.provider),
        );
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
  if (error)
    return (
      <div className="workspace-empty" role="alert">
        <strong>{t("读取行情数据失败")}</strong>
        <ErrorNotice error={error} />
        <button className="primary" onClick={() => setRetry(value => value + 1)}>
          {t("重试")}
        </button>
      </div>
    );
  return (
    <div className="page-body data-body">
      <RecordingStatus context={context} />
      <RecordedDays />
      <MinuteComparison />
      <div className="data-source">
        <label>
          {t("数据源")}
          <select value={provider} onChange={event => setProvider(event.target.value)}>
            {sources.map(item => (
              <option key={item.provider} value={item.provider}>
                {item.provider === "recorder"
                  ? t("自录")
                  : item.provider === "research"
                    ? t("研究数据")
                    : item.provider === "rules"
                      ? t("导入规则")
                      : item.provider}
              </option>
            ))}
          </select>
        </label>
        <p className="subtle">
          {t(
            "各来源分别采集和保存。研究只读「研究数据」：行情服务按规则合成，日线和分钟线以 Tushare 为准，Tushare 没有的合约交易日用自录分钟线。",
          )}
        </p>
      </div>
      {canManage && (
        <PriceTickImport
          onImported={dataset => {
            setImported(dataset);
            setProvider(dataset.provider);
            setRetry(value => value + 1);
          }}
        />
      )}
      {!source && canManage !== null && (
        <p className="data-card">{t("行情服务尚未启用采集来源，也没有已保存的数据集。")}</p>
      )}
      {source && (
        <SourceCoverage
          key={`${provider}/${source.tables.join()}/${source.exchanges.join()}/${imported?.version ?? opened?.version}`}
          source={source}
          canManage={canManage === true}
          opened={
            imported?.provider === provider
              ? imported.version
              : opened?.provider === provider
                ? (opened.version ?? "")
                : ""
          }
        />
      )}
    </div>
  );
}

function SourceCoverage({
  source,
  canManage,
  opened,
}: {
  source: Source;
  canManage: boolean;
  opened: string;
}) {
  const provider = source.provider;
  // The version shown; empty for the current one.
  const [viewed, setViewed] = useState(opened);
  const [latest, setLatest] = useState("");
  const [change, setChange] = useState<MarketChange>();
  const [table, setTable] = useState(source.tables[0] ?? Table.UNSPECIFIED);
  const [exchange, setExchange] = useState(source.exchanges[0] ?? "");
  const [contracts, setContracts] = useState<string[]>([]);
  const catalog = useMarket()?.board.catalog?.contracts;
  const [replace, setReplace] = useState(false);
  const [summary, setSummary] = useState<MarketSummary | null>(null);
  const [coverage, setCoverage] = useState<MarketCoverage>();
  const [tasks, setTasks] = useState<MarketTask[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [begin, setBegin] = useState(() => day(-30));
  const [end, setEnd] = useState(() => day(-1));
  const [submitting, setSubmitting] = useState(false);
  const [submitted, setSubmitted] = useState("");
  const started = useActionError();
  const cancelled = useActionError();
  const mounted = useRef(false);
  const pending = useRef(false);
  // Tasks to show: one page, more on request.
  const shown = useRef(20);
  const [older, setOlder] = useState(false);

  const refresh = useCallback(async () => {
    if (pending.current) return;
    pending.current = true;
    try {
      const stored = await marketData({ case: "listDatasets", value: {} });
      const nextLatest = stored.datasets.find(item => item.provider === provider)?.version ?? "";
      const nextSummary = await marketData({
        case: "getDatasetSummary",
        value: { provider, version: viewed },
      });
      const version = nextSummary.dataset?.version;
      const nextCoverage = version
        ? await marketData({ case: "getDatasetCoverage", value: { provider, version } })
        : undefined;
      // Market lists this source's tasks newest first; read as many pages as shown.
      const nextTasks: MarketTask[] = [];
      let afterId = "";
      do {
        const page = await marketData({
          case: "listAcquisitionTasks",
          value: { provider, afterId },
        });
        nextTasks.push(...page.tasks);
        afterId = page.nextId;
      } while (afterId && nextTasks.length < shown.current && mounted.current);
      if (!mounted.current) return;
      setOlder(afterId !== "");
      setLatest(nextLatest);
      setSummary(nextSummary);
      setCoverage(nextCoverage);
      setTasks(nextTasks);
      setError("");
    } catch (failure) {
      if (mounted.current) setError(asDisplayError(failure));
    } finally {
      pending.current = false;
    }
  }, [provider, viewed]);
  useEffect(() => {
    let active = true;
    setChange(undefined);
    if (viewed && latest && viewed !== latest)
      void marketData({
        case: "compareDatasetVersions",
        value: { provider, version: latest, base: viewed },
      })
        .then(next => active && setChange(next))
        .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [provider, viewed, latest]);
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
  // The reference tables this version holds rows of.
  const stored = referenceTables.filter(table =>
    summary?.tables.some(item => item.table === table && item.rows > 0n),
  );
  // Contracts a minute task can choose from: those Market lists now and those
  // this source already holds minutes of.
  const known = useMemo(
    () => [
      ...new Set([
        ...(catalog ?? []).map(item => item.contract),
        ...(coverage?.contracts ?? []).map(item => item.contract),
      ]),
    ],
    [catalog, coverage],
  );
  const form = useRef<HTMLElement>(null);
  const fillsDaily = canManage && source.tables.includes(Table.DAILY);
  // Fills the form with one coverage gap; stored partitions are skipped, so the
  // whole month can be asked for.
  const fill = (
    next: Table,
    month: string,
    target: { exchange?: string; contracts?: string[] },
  ) => {
    const [first, last] = monthRange(month);
    setTable(next);
    if (target.exchange) setExchange(target.exchange);
    if (target.contracts) setContracts(target.contracts);
    setBegin(first);
    setEnd(last);
    form.current?.scrollIntoView({ block: "nearest" });
  };
  const acquire = async () => {
    setSubmitting(true);
    const id = `${provider}-${crypto.randomUUID()}`;
    setSubmitted(id);
    await started.attempt(async () => {
      await marketData({
        case: "startAcquisition",
        value: {
          id,
          provider,
          table,
          refetch: replace,
          // A minute task's exchanges follow from its contracts.
          exchanges: table === Table.MINUTES ? [] : [exchange],
          contracts: table === Table.MINUTES ? contracts : [],
          firstDay: begin,
          lastDay: end,
        },
      });
      await refresh();
    });
    if (mounted.current) setSubmitting(false);
  };
  const cancel = (id: string) =>
    cancelled.attempt(async () => {
      await marketData({ case: "cancelAcquisition", value: { id } });
      await refresh();
    });
  const daily = useMemo(() => coverage?.products ?? [], [coverage]);
  const months = useMemo(
    () => [...new Set(daily.flatMap(item => item.months.map(m => m.month)))].sort(),
    [daily],
  );
  return (
    <>
      <section className="data-card" aria-label={t("数据集版本")}>
        <header>
          <h3>{t("数据集版本")}</h3>
          {summary?.dataset?.version && (
            <code title={summary.dataset.version}>{summary.dataset.version.slice(0, 12)}</code>
          )}
        </header>
        <ErrorNotice error={error} />
        {viewed && viewed !== latest && (
          <p role="status">
            {t("正在查看旧版本。当前版本与它相比：")}
            {change ? changeText(change) : t("读取中")}{" "}
            <button type="button" onClick={() => setViewed("")}>
              {t("回到当前版本")}
            </button>
          </p>
        )}
        {summary?.dataset?.version ? (
          <table className="data-table">
            <thead>
              <tr>
                <th>{t("表")}</th>
                <th className="numeric">{t("行数")}</th>
                <th className="numeric">{t("分区")}</th>
                <th>{t("日期范围")}</th>
              </tr>
            </thead>
            <tbody>
              {summary.tables.map(item => (
                <tr key={item.table}>
                  <td>{tableName(item.table)}</td>
                  <td className="numeric">{item.rows.toString()}</td>
                  <td className="numeric">{item.partitions.toString()}</td>
                  <td>{item.firstDay ? `${item.firstDay} – ${item.lastDay}` : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        ) : (
          <p className="subtle">{t("仓库里还没有数据。")}</p>
        )}
      </section>
      <DatasetHistory provider={provider} latest={latest} viewed={viewed} onView={setViewed} />
      {source.tables.length > 0 && (
        <section
          ref={form}
          className="data-card"
          aria-label={t(source.provider === "recorder" ? "发布录制分钟" : "采集")}
        >
          <header>
            <h3>{t(source.provider === "recorder" ? "发布录制分钟" : "采集")}</h3>
          </header>
          <p className="subtle">
            {t(
              source.provider === "recorder"
                ? "将行情服务已落盘的分钟发布为研究版本，保留不完整分钟和观测中断；补录后可重新发布，旧版本仍可读取。"
                : "采集按交易所保存，分钟线按合约保存；已入库分区跳过。",
            )}{" "}
            {t("来源配置和凭据由行情服务主机管理；关闭此页面不会停止采集。")}
          </p>
          <div className="data-fields">
            <label>
              {t("表")}
              <select value={table} onChange={event => setTable(Number(event.target.value))}>
                {source.tables.map(item => (
                  <option key={item} value={item}>
                    {tableName(item)}
                  </option>
                ))}
              </select>
            </label>
            {table !== Table.MINUTES && (
              <label>
                {t("交易所")}
                <select value={exchange} onChange={event => setExchange(event.target.value)}>
                  {source.exchanges.map(item => (
                    <option key={item}>{item}</option>
                  ))}
                </select>
              </label>
            )}
            <label>
              {t("开始日期")}
              <input type="date" value={begin} onChange={event => setBegin(event.target.value)} />
            </label>
            <label>
              {t("结束日期")}
              <input type="date" value={end} onChange={event => setEnd(event.target.value)} />
            </label>
          </div>
          {table === Table.MINUTES && (
            <ContractPicker known={known} value={contracts} onChange={setContracts} />
          )}
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
          <div className="data-actions">
            <button
              type="button"
              className="primary"
              disabled={
                !canManage ||
                submitting ||
                running ||
                !begin ||
                !end ||
                begin > end ||
                !table ||
                (table === Table.MINUTES ? !contracts.length : !exchange)
              }
              onClick={() => void acquire()}
            >
              {t(source.provider === "recorder" ? "发布录制分钟" : "开始采集")}
            </button>
            {!canManage && (
              <span className="subtle" role="status">
                {t("当前连接只有读取权限，不能启动或取消采集。")}
              </span>
            )}
            {submitted && (
              <span className="subtle">
                {t("本次提交任务")}: <code>{submitted}</code>
              </span>
            )}
          </div>
          <ActionError error={started.error} />
        </section>
      )}
      {tasks.length > 0 && (
        <section className="data-card" aria-label={t("采集任务")}>
          <header>
            <h3>{t("采集任务")}</h3>
          </header>
          <ActionError error={cancelled.error} />
          <table className="data-table" aria-label={t("采集任务")}>
            <thead>
              <tr>
                <th>{t("任务编号")}</th>
                <th>{t("表")}</th>
                <th>{t("区间")}</th>
                <th>{t("状态")}</th>
                <th>{t("进度")}</th>
                <th className="numeric">{t("请求次数")}</th>
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
                    {task.request!.firstDay} – {task.request!.lastDay}
                  </td>
                  <td title={task.error?.message} data-state={TaskState[task.state].toLowerCase()}>
                    {t(states[task.state] ?? String(task.state))}
                    {task.error ? ` · ${task.error.message}` : ""}
                  </td>
                  <td>
                    {task.itemsDone.toString()} / {task.items.toString()}
                    {task.itemsEmpty > 0n
                      ? ` · ${t("{count} 天来源尚无数据", { count: Number(task.itemsEmpty) })}`
                      : ""}
                  </td>
                  <td className="numeric">{task.requestsSent.toString()}</td>
                  <td>
                    {canManage && activeTask(task) && (
                      <button onClick={() => void cancel(task.request!.id)}>{t("取消")}</button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          {older && (
            <button
              type="button"
              onClick={() => {
                shown.current += 20;
                void refresh();
              }}
            >
              {t("更早的任务")}
            </button>
          )}
        </section>
      )}
      <section className="data-card" aria-label={t("日线覆盖")}>
        <header>
          <h3>{t("日线覆盖")}</h3>
          <span className="subtle">
            {t("每格为该来源已存日线的天数；没有交易日历时，完整性未知。")}
          </span>
        </header>
        {daily.length ? (
          <div className="coverage-matrix">
            <table className="data-table">
              <thead>
                <tr>
                  <th>{t("品种")}</th>
                  {months.map(month => (
                    <th key={month}>{month}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {daily.map(product => {
                  const byMonth = new Map(product.months.map(m => [m.month, m]));
                  return (
                    <tr key={product.product}>
                      <th scope="row">{product.product}</th>
                      {months.map(month => {
                        const cell = byMonth.get(month);
                        const share = cell?.openDays ? cell.days / cell.openDays : 0;
                        return (
                          <td
                            key={month}
                            data-share={cell ? (share >= 1 ? "full" : "partial") : "none"}
                            title={cell ? `${cell.days} / ${cell.openDays || "?"}` : t("无数据")}
                          >
                            {fillsDaily && share < 1 ? (
                              <button
                                type="button"
                                aria-label={t("补采 {target} {month}", {
                                  target: product.product,
                                  month,
                                })}
                                onClick={() =>
                                  fill(Table.DAILY, month, {
                                    exchange: product.product.split("/")[0]!,
                                  })
                                }
                              >
                                {cell ? cell.days : "+"}
                              </button>
                            ) : cell ? (
                              cell.days
                            ) : (
                              ""
                            )}
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
          <p className="subtle">{t("仓库里还没有日线。")}</p>
        )}
      </section>
      {stored.length > 0 && (
        <ReferenceTables
          key={summary!.dataset!.version}
          provider={provider}
          version={summary!.dataset!.version}
          tables={stored}
          exchanges={source.exchanges.length ? source.exchanges : exchanges}
        />
      )}
      <MinuteCoverage
        contracts={coverage?.contracts ?? []}
        onFill={
          canManage && source.tables.includes(Table.MINUTES)
            ? (contract, month) => fill(Table.MINUTES, month, { contracts: [contract] })
            : undefined
        }
      />
    </>
  );
}
