import { Table, TaskState } from "../../src/generated/asterion/history_pb";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ContractPicker } from "./ContractPicker";
import { DatasetHistory, changeText } from "./DatasetHistory";
import { MinuteCoverage } from "./MinuteCoverage";
import { useMarket } from "../../src/host/market";
import { MarketStatus } from "./MarketStatus";
import { ReferenceTables, referenceTables } from "./ReferenceTables";
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
  type AcquisitionOptions,
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
  [Table.PRICE_TICKS]: "历史报价单位",
};
const tableName = (table: Table) => t(tables[table] ?? Table[table]);
const exchanges = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];

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

// Another page can open a version: param `version`.
function DataPage({ context }: { context: TerminalContext }) {
  const opened = context.workspaceParams;
  const [options, setOptions] = useState<AcquisitionOptions>();
  const [error, setError] = useState<DisplayError>("");
  const [retry, setRetry] = useState(0);
  useEffect(() => {
    let active = true;
    marketData({ case: "getAcquisitionOptions", value: {} })
      .then(next => {
        if (!active) return;
        setOptions(next);
        setError("");
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [retry]);
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
      <MarketStatus context={context} />
      {options && (
        <DataCoverage
          key={`${options.tables.join()}/${options.exchanges.join()}/${opened?.version}`}
          options={options}
          opened={opened?.version ?? ""}
        />
      )}
    </div>
  );
}

function DataCoverage({ options, opened }: { options: AcquisitionOptions; opened: string }) {
  const canManage = options.canManage;
  // The version shown; empty for the current one.
  const [viewed, setViewed] = useState(opened);
  const [latest, setLatest] = useState("");
  const [change, setChange] = useState<MarketChange>();
  const [table, setTable] = useState(options.tables[0] ?? Table.UNSPECIFIED);
  const [exchange, setExchange] = useState(options.exchanges[0] ?? "");
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
      const current = await marketData({ case: "getDatasetSummary", value: {} });
      const nextLatest = current.dataset?.version ?? "";
      const nextSummary = viewed
        ? await marketData({ case: "getDatasetSummary", value: { version: viewed } })
        : current;
      const version = nextSummary.dataset?.version;
      const nextCoverage = version
        ? await marketData({ case: "getDatasetCoverage", value: { version } })
        : undefined;
      // Market lists tasks newest first; read as many pages as shown.
      const nextTasks: MarketTask[] = [];
      let afterId = "";
      do {
        const page = await marketData({
          case: "listAcquisitionTasks",
          value: { afterId },
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
  }, [viewed]);
  useEffect(() => {
    let active = true;
    setChange(undefined);
    if (viewed && latest && viewed !== latest)
      void marketData({
        case: "compareDatasetVersions",
        value: { version: latest, base: viewed },
      })
        .then(next => active && setChange(next))
        .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [viewed, latest]);
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
  // The reference tables this version holds rows of.
  const stored = referenceTables.filter(table =>
    summary?.tables.some(item => item.table === table && item.rows > 0n),
  );
  // Contracts a minute task can choose from: those Market lists now and those
  // the dataset already holds minutes of.
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
  const fillsDaily = canManage && options.tables.includes(Table.DAILY);
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
    const id = crypto.randomUUID();
    setSubmitted(id);
    await started.attempt(async () => {
      await marketData({
        case: "startAcquisition",
        value: {
          id,
          table,
          refetch: replace,
          // A minute task's exchanges follow from its contracts.
          exchanges: table === Table.MINUTES ? [] : [exchange],
          contracts: table === Table.MINUTES ? contracts : [],
          firstDay: begin,
          lastDay: end,
        },
      });
      // Market data is usable only with its price ticks.
      if (
        [Table.DAILY, Table.MINUTES, Table.SETTLEMENT].includes(table) &&
        options.tables.includes(Table.PRICE_TICKS)
      )
        await marketData({
          case: "startAcquisition",
          value: {
            id: crypto.randomUUID(),
            table: Table.PRICE_TICKS,
            refetch: replace,
            exchanges:
              table === Table.MINUTES
                ? [...new Set(contracts.map(contract => contract.split("/")[0]!))]
                : [exchange],
            contracts: [],
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
      <DatasetHistory latest={latest} viewed={viewed} onView={setViewed} />
      {options.tables.length > 0 && (
        <section ref={form} className="data-card" aria-label={t("采集")}>
          <header>
            <h3>{t("采集")}</h3>
          </header>
          <p className="subtle">
            {t("采集按交易所保存，分钟线按合约保存；已入库分区跳过。")}{" "}
            {t("行情服务以 Tushare 为准，缺的部分从公开来源补齐；关闭此页面不会停止采集。")}
          </p>
          <div className="data-fields">
            <label>
              {t("表")}
              <select value={table} onChange={event => setTable(Number(event.target.value))}>
                {options.tables.map(item => (
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
                  {options.exchanges.map(item => (
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
            {t("重新采集已有分区，保留旧数据集版本")}
          </label>
          <div className="data-actions">
            <button
              type="button"
              className="primary"
              disabled={
                !canManage ||
                submitting ||
                !begin ||
                !end ||
                begin > end ||
                !table ||
                (table === Table.MINUTES ? !contracts.length : !exchange)
              }
              onClick={() => void acquire()}
            >
              {t("开始采集")}
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
          version={summary!.dataset!.version}
          tables={stored}
          exchanges={options.exchanges.length ? options.exchanges : exchanges}
        />
      )}
      <MinuteCoverage
        contracts={coverage?.contracts ?? []}
        onFill={
          canManage && options.tables.includes(Table.MINUTES)
            ? (contract, month) => fill(Table.MINUTES, month, { contracts: [contract] })
            : undefined
        }
      />
    </>
  );
}
