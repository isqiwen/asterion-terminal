import { Table } from "../../src/generated/asterion/warehouse_pb";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Acquisition, type Preset } from "./Acquisition";
import { DailyCoverage } from "./DailyCoverage";
import { DatasetHistory, changeText } from "./DatasetHistory";
import { Dialog } from "./Dialog";
import { MinuteCoverage } from "./MinuteCoverage";
import { useMarket } from "../../src/host/market";
import { ReferenceTable } from "./ReferenceTable";
import { StoredContracts } from "./StoredContracts";
import { Tasks, activeTask } from "./Tasks";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  warehouseData,
  translate,
  type DisplayError,
  type MarketChange,
  type MarketCoverage,
  type MarketSummary,
  type MarketTask,
  type AcquisitionOptions,
  type MessageValues,
  type TerminalContext,
  MarketUnset,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const day = (offset: number) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
// A month's days up to yesterday: providers only hold finished trading days.
const monthRange = (month: string) => {
  const [year, number] = month.split("-").map(Number);
  const last = new Date(Date.UTC(year!, number!, 0)).toISOString().slice(0, 10);
  return { first: `${month}-01`, last: last < day(-1) ? last : day(-1) };
};
const tables: Partial<Record<Table, string>> = {
  [Table.DAILY]: "日线",
  [Table.MINUTES]: "分钟线",
  [Table.SETTLEMENT]: "结算参数",
  [Table.PRICE_TICKS]: "历史报价单位",
  [Table.CONTRACTS]: "合约表",
  [Table.CALENDAR]: "交易日历",
};
// One view per table, the stored contracts' charts, then every client's tasks.
type View = Table | "stored" | "tasks";
const views: View[] = [
  Table.DAILY,
  Table.MINUTES,
  Table.SETTLEMENT,
  Table.PRICE_TICKS,
  Table.CONTRACTS,
  Table.CALENDAR,
  "stored",
  "tasks",
];
const tableName = (table: Table) => t(tables[table] ?? Table[table]);
const viewName = (view: View) =>
  view === "stored" ? t("已采集合约") : view === "tasks" ? t("采集任务") : tableName(view);
const exchanges = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];

// Every read and acquisition goes to the saved remote Warehouse service. Browsing a
// source changes only this component, never the service's collection lifecycle.
export function Coverage(context: TerminalContext) {
  const saved = context.snapshot?.warehouse_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  return (
    <section className="data-page" aria-label={t("数据")}>
      {endpoint ? (
        <DataPage
          key={`${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`}
          context={context}
        />
      ) : (
        <MarketUnset openSettings={context.openSettings} page="warehouse" />
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
    warehouseData({ case: "getAcquisitionOptions", value: {} })
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
        <strong>{t("读取历史数据失败")}</strong>
        <ErrorNotice error={error} />
        <button className="primary" onClick={() => setRetry(value => value + 1)}>
          {t("重试")}
        </button>
      </div>
    );
  return (
    <div className="data-body">
      {options && (
        <DataCoverage
          key={`${options.tables.join()}/${options.exchanges.join()}/${opened?.version}`}
          options={options}
          opened={opened?.version ?? ""}
          context={context}
        />
      )}
    </div>
  );
}

function DataCoverage({
  options,
  opened,
  context,
}: {
  options: AcquisitionOptions;
  opened: string;
  context: TerminalContext;
}) {
  const canManage = options.canManage;
  // The version shown; empty for the current one.
  const [viewed, setViewed] = useState(opened);
  const [latest, setLatest] = useState("");
  const [change, setChange] = useState<MarketChange>();
  const [view, setView] = useState<View>(Table.DAILY);
  const [acquiring, setAcquiring] = useState<Preset>();
  const [versionsOpen, setVersionsOpen] = useState(false);
  const catalog = useMarket()?.board.catalog?.contracts;
  const [summary, setSummary] = useState<MarketSummary | null>(null);
  const [coverage, setCoverage] = useState<MarketCoverage>();
  const [tasks, setTasks] = useState<MarketTask[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const mounted = useRef(false);
  const pending = useRef(false);
  // Tasks to show: one page, more on request.
  const shown = useRef(20);
  const [older, setOlder] = useState(false);

  const refresh = useCallback(async () => {
    if (pending.current) return;
    pending.current = true;
    try {
      const current = await warehouseData({ case: "getDatasetSummary", value: {} });
      const nextLatest = current.dataset?.version ?? "";
      const nextSummary = viewed
        ? await warehouseData({ case: "getDatasetSummary", value: { version: viewed } })
        : current;
      const version = nextSummary.dataset?.version;
      const nextCoverage = version
        ? await warehouseData({ case: "getDatasetCoverage", value: { version } })
        : undefined;
      // Warehouse lists tasks newest first; read as many pages as shown.
      const nextTasks: MarketTask[] = [];
      let afterId = "";
      do {
        const page = await warehouseData({
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
      void warehouseData({
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
    const timer = setInterval(() => void refresh(), 3000);
    return () => {
      mounted.current = false;
      clearInterval(timer);
    };
  }, [refresh]);
  // Contracts an acquisition can choose from: those Market lists now and those
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
  const version = summary?.dataset?.version ?? "";
  const running = tasks.filter(activeTask).length;
  const acquires = typeof view === "number" && options.tables.includes(view);
  const fills = canManage && acquires;
  const stats = (table: Table) => summary?.tables.find(item => item.table === table);
  const meta = (table: Table) => {
    const item = stats(table);
    if (!item?.rows) return "—";
    const rows = t("{rows} 行", { rows: Number(item.rows).toLocaleString(getLocale()) });
    return item.firstDay ? `${rows} · ${item.firstDay} – ${item.lastDay}` : rows;
  };
  return (
    <>
      <ErrorNotice error={error} />
      <div className="data-main">
        <nav className="data-views" aria-label={t("表")}>
          {/* Every table below belongs to this one version of the dataset. */}
          <section className="data-version" aria-label={t("数据集版本")}>
            <button
              type="button"
              disabled={!version}
              data-earlier={(viewed && viewed !== latest) || undefined}
              onClick={() => setVersionsOpen(true)}
            >
              {t("数据版本")} {version ? <code title={version}>{version.slice(0, 8)}</code> : "—"}
              {viewed && viewed !== latest && ` · ${t("旧版本")}`}
            </button>
            {viewed && viewed !== latest && (
              <p role="status">
                {t("当前版本与它相比：")}
                {change ? changeText(change) : t("读取中")}{" "}
                <button type="button" onClick={() => setViewed("")}>
                  {t("回到当前版本")}
                </button>
              </p>
            )}
          </section>
          {views.map(item => (
            <button
              key={item}
              type="button"
              aria-current={view === item || undefined}
              onClick={() => setView(item)}
            >
              <span>{viewName(item)}</span>
              {typeof item === "number" && <small>{meta(item)}</small>}
              {item === "tasks" && running > 0 && (
                <small>{t("{count} 进行中", { count: running })}</small>
              )}
            </button>
          ))}
        </nav>
        <section className="data-view" aria-label={viewName(view)}>
          <header>
            <h3>{viewName(view)}</h3>
            {acquires &&
              (canManage ? (
                <button type="button" className="primary" onClick={() => setAcquiring({})}>
                  {t("采集")}
                </button>
              ) : (
                <span className="subtle">{t("只读连接，不能采集或取消任务")}</span>
              ))}
          </header>
          {view === Table.DAILY ? (
            <DailyCoverage
              products={coverage?.products ?? []}
              onFill={
                fills
                  ? (product, month) =>
                      setAcquiring({ exchange: product.split("/")[0]!, ...monthRange(month) })
                  : undefined
              }
            />
          ) : view === Table.MINUTES ? (
            <MinuteCoverage
              contracts={coverage?.contracts ?? []}
              onFill={
                fills
                  ? (contract, month) =>
                      setAcquiring({ instruments: [contract], ...monthRange(month) })
                  : undefined
              }
            />
          ) : view === "stored" ? (
            latest && <StoredContracts key={latest} context={context} />
          ) : view === "tasks" ? (
            <Tasks
              tasks={tasks}
              tableName={tableName}
              canManage={canManage}
              refresh={refresh}
              older={
                older
                  ? () => {
                      shown.current += 20;
                      void refresh();
                    }
                  : undefined
              }
            />
          ) : version && stats(view)?.rows ? (
            <ReferenceTable
              key={`${version}/${view}`}
              version={version}
              table={view}
              name={tableName(view)}
              exchanges={options.exchanges.length ? options.exchanges : exchanges}
            />
          ) : (
            <p className="subtle">{t("仓库里还没有这张表。")}</p>
          )}
        </section>
      </div>
      {acquiring && typeof view === "number" && (
        <Acquisition
          table={view}
          name={tableName(view)}
          options={options}
          known={known}
          preset={acquiring}
          close={() => setAcquiring(undefined)}
          started={() => {
            setAcquiring(undefined);
            setView("tasks");
            void refresh();
          }}
        />
      )}
      {versionsOpen && (
        <Dialog title={t("版本历史")} close={() => setVersionsOpen(false)}>
          <DatasetHistory
            latest={latest}
            viewed={viewed}
            onView={next => {
              setViewed(next);
              setVersionsOpen(false);
            }}
          />
          <div className="data-actions">
            <button type="button" onClick={() => setVersionsOpen(false)}>
              {t("关闭")}
            </button>
          </div>
        </Dialog>
      )}
    </>
  );
}
