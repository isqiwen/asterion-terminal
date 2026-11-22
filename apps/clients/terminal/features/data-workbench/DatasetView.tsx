import { useEffect, useMemo, useRef, useState } from "react";
import { lab } from "../../src/bridge/lab";
import { warehouseData } from "../../src/bridge/warehouse-data";
import {
  Table,
  type AcquisitionTask,
  type DatasetChange,
  type DatasetCoverage,
} from "../../src/generated/asterion/warehouse_pb";
import { useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarket } from "../../src/host/market";
import { useTradingAccounts } from "../../src/host/trading-accounts";
import { useErrorNotification } from "../../src/host/notifications";
import { useWarehouse, useWarehousePoll } from "../../src/host/warehouse";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { getLocale } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { knownProducts, warehouseName } from "../../src/ui/contract-names";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { Acquisition, type Preset } from "./Acquisition";
import { Activity, activeTask } from "./Activity";
import { VersionBanner, WarehousePage, WarehouseUnavailable } from "./Page";
import { beijingDay, exchanges, freshness, t, tableName } from "./dataset";

// A month's days up to yesterday: providers only hold finished trading days.
const monthRange = (month: string) => {
  const [year, number] = month.split("-").map(Number);
  const first = `${month}-01`;
  const monthEnd = new Date(Date.UTC(year!, number!, 0)).toISOString().slice(0, 10);
  const yesterday = beijingDay(-1);
  const capped = monthEnd < yesterday ? monthEnd : yesterday;
  return { first, last: capped < first ? first : capped };
};
const monthsOf = (year: string) =>
  Array.from({ length: 12 }, (_, index) => `${year}-${String(index + 1).padStart(2, "0")}`);
const yearSpan = (high: number, low: number) =>
  Array.from({ length: high - low + 1 }, (_, index) => String(high - index));
const earliestOfferedYear = 2010;

// One row of the matrix: a product's daily bars or a contract's minutes. Open
// days count through yesterday and from the row's first day.
type Month = {
  days: number;
  open: number;
  empty: number;
  pending: number;
};
type Row = {
  id: string;
  name?: string;
  exchange: string;
  months: Map<string, Month>;
};
type Cell =
  | "full"
  | "partial"
  | "missing"
  | "pending"
  | "empty"
  | "held"
  | "unacquired"
  | "delisted"
  | "future";

const gaps = (cell: Month) => Math.max(0, cell.open - cell.days - cell.empty - cell.pending);

function cellOf(
  row: Row,
  month: string,
  first: string | undefined,
  last: string | undefined,
  latestMonth: string,
): Cell {
  const cell = row.months.get(month);
  if (!cell) {
    if (first && last && month > first && month < last) return "missing";
    const delivery = row.id.split("/")[2];
    if (delivery && month > delivery) return "delisted";
    if (month > latestMonth) return "future";
    return "unacquired";
  }
  if (!cell.open) return "held";
  if (gaps(cell)) return cell.days || cell.pending ? "partial" : "missing";
  if (cell.pending) return "pending";
  return cell.empty ? "empty" : "full";
}

function cellText(cell: Month | undefined, state: Cell, interactive: boolean) {
  if (state === "missing") return interactive ? "+" : "—";
  if (state === "delisted") return "—";
  if (state === "future") return "";
  if (!cell) return state === "unacquired" && interactive ? "+" : "";
  if (state === "partial" || state === "pending") return `${cell.days}/${cell.open}`;
  return String(cell.days);
}

// The states worth a key: months not acquired or not yet here stay blank.
const legend = [
  ["full", "齐全"],
  ["pending", "待更新"],
  ["partial", "有缺口"],
  ["missing", "整月缺失"],
  ["empty", "来源无数据"],
  ["delisted", "已摘牌"],
] as const;

function rowsOf(coverage: DatasetCoverage, minutes: boolean): Row[] {
  const month = (m: {
    month: string;
    days: number;
    openDays: number;
    emptyDays: number;
    pendingDays: number;
  }) =>
    [
      m.month,
      {
        days: m.days,
        open: m.openDays,
        empty: m.emptyDays,
        pending: m.pendingDays,
      },
    ] as const;
  return minutes
    ? coverage.contracts.map(item => ({
        id: item.contract,
        name: warehouseName(item.contract),
        exchange: item.contract.split("/")[0]!,
        months: new Map(item.months.map(month)),
      }))
    : coverage.products.map(item => ({
        id: item.product,
        name: warehouseName(item.product),
        exchange: item.product.split("/")[0]!,
        months: new Map(item.months.map(month)),
      }));
}

// Only mixed causes or days the source never had need more than the number and colour.
function describe(cell: Month) {
  const missing = gaps(cell);
  if (!cell.empty && !(missing && cell.pending)) return undefined;
  return [
    `${cell.days} / ${cell.open}`,
    missing && t("缺 {days} 天", { days: missing }),
    cell.pending && t("{days} 天待更新", { days: cell.pending }),
    cell.empty && t("{days} 天来源无数据", { days: cell.empty }),
  ]
    .filter(Boolean)
    .join(" · ");
}

// What the research dataset holds for daily and minute bars, where any gaps sit
// across products and months, and the acquisitions and versions behind it.
export function Dataset(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据覆盖")}>
      {() => <DatasetBody context={context} />}
    </WarehousePage>
  );
}

function DatasetBody({ context }: { context: TerminalContext }) {
  const paramVersion = context.workspaceParams?.version ?? "";
  const [viewed, setViewed] = useState(paramVersion);
  const [kind, setKind] = useWorkspaceDraft<"daily" | "minutes">("coverage-kind", "daily");
  const paramKind = context.workspaceParams?.kind;
  useEffect(() => {
    if (!paramVersion && !paramKind) return;
    if (paramVersion) setViewed(paramVersion);
    if (paramKind === "daily" || paramKind === "minutes") setKind(paramKind);
    context.navigate("workspace.data-dataset", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramVersion, paramKind]);
  const [exchange, setExchange] = useWorkspaceDraft("coverage-exchange", "");
  const [search, setSearch] = useWorkspaceDraft("coverage-search", "");
  const [chosenYear, setYear] = useState("");
  const [shown, setShown] = useState(20);
  const [everything, setEverything] = useState(false);
  const [backtested, setBacktested] = useState<readonly string[]>([]);
  useEffect(() => {
    // Without Lab, recent backtests add nothing to the products shown.
    void lab({ case: "listRecentRuns", value: {} })
      .then(reply => setBacktested(reply.runs.flatMap(run => run.symbols)))
      .catch(() => {});
  }, []);
  const { views } = useTradingAccounts();
  const [acquiring, setAcquiring] = useState<{ table: Table; preset: Preset }>();
  const catalog = useMarket()?.board.catalog?.contracts;
  const warehouse = useWarehouse();
  const warehouseRef = useRef(warehouse);
  warehouseRef.current = warehouse;
  const options = useWarehousePoll(
    () => warehouseData({ case: "getAcquisitionOptions", value: {} }),
    [],
    true,
  );
  const state = useWarehousePoll(async () => {
    const current = await warehouseData({ case: "getDatasetSummary", value: {} });
    const latest = current.dataset?.version ?? "";
    const version = viewed || latest;
    const summary =
      viewed && viewed !== latest
        ? await warehouseData({ case: "getDatasetSummary", value: { version: viewed } })
        : current;
    const coverage = version
      ? await warehouseData({ case: "getDatasetCoverage", value: { version } })
      : undefined;
    const tasks: AcquisitionTask[] = [];
    let afterId = "";
    const shared = warehouseRef.current;
    const useSharedTasks =
      !viewed &&
      shared?.tasks !== undefined &&
      (!shared.nextTaskId || shared.tasks.length >= shown);
    if (!viewed && !useSharedTasks) {
      if (shared?.tasks) {
        tasks.push(...shared.tasks);
        afterId = shared.nextTaskId;
      }
      while (!tasks.length || (afterId && tasks.length < shown)) {
        const page = await warehouseData({ case: "listAcquisitionTasks", value: { afterId } });
        tasks.push(...page.tasks);
        afterId = page.nextId;
        if (!afterId) break;
      }
    }
    return { latest, summary, coverage, tasks, older: afterId !== "" };
  }, [viewed, shown]);
  const refreshAll = async () => {
    await warehouse?.refresh();
    await state.refresh();
  };
  const latest = state.value?.latest ?? "";
  const [versions, setVersions] = useState<{ items: DatasetChange[]; older: boolean }>();
  const [versionError, setVersionError] = useState<DisplayError>("");
  useEffect(() => {
    if (viewed) return;
    let active = true;
    void (async () => {
      const items: DatasetChange[] = [];
      let after = "";
      do {
        const page = await warehouseData({
          case: "listDatasetVersions",
          value: { afterVersion: after },
        });
        items.push(...page.versions);
        after = page.nextVersion;
      } while (after && items.length < shown);
      if (active) setVersions({ items, older: after !== "" });
    })().catch(reason => active && setVersionError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [latest, shown, viewed]);

  const update = useActionError();
  useErrorNotification(update.error, "asterion.terminal.data-workbench");
  const [updated, setUpdated] = useState("");
  const startUpdate = () =>
    update.attempt(async () => {
      setUpdated("");
      const reply = await warehouseData({ case: "startUpdate", value: {} });
      const currentTasks = warehouse?.tasks ?? state.value?.tasks ?? [];
      const hasGaps = state.value?.summary.tables.some(item => item.missing > 0n);
      const hasActive = currentTasks.some(activeTask);
      setUpdated(
        reply.tasks.length
          ? t("已提交 {count} 个更新任务", { count: reply.tasks.length })
          : hasGaps && !hasActive
            ? t("各表末日已到最新；中间缺口请点击对应缺口或“采集”补齐")
            : t("各表已是最新，或已有任务在进行"),
      );
      await refreshAll();
    });

  const minutes = kind === "minutes";
  const table = minutes ? Table.MINUTES : Table.DAILY;
  const coverage = state.value?.coverage;
  const offered = options.value?.tables.map(item => item.table) ?? [];
  const offeredExchanges = useMemo(
    () => options.value?.tables.find(item => item.table === table)?.exchanges ?? [],
    [options.value?.tables, table],
  );
  const acquires = !viewed && offered.includes(table);
  const locale = getLocale();
  const storedRows = useMemo(
    () => (coverage ? rowsOf(coverage, minutes) : []),
    // warehouseName depends on the current locale.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [coverage, minutes, locale],
  );
  const storedYears = [
    ...new Set(
      storedRows.flatMap(row => [...row.months.keys()].map(month => Number(month.slice(0, 4)))),
    ),
  ].sort((a, b) => b - a);
  const latestYear = Number(beijingDay(-1).slice(0, 4));
  const years = acquires
    ? yearSpan(
        Math.max(latestYear, ...(storedYears.length ? storedYears : [latestYear])),
        Math.min(
          earliestOfferedYear,
          ...(storedYears.length ? storedYears : [earliestOfferedYear]),
        ),
      )
    : storedYears.length
      ? yearSpan(storedYears[0]!, storedYears.at(-1)!)
      : [String(latestYear)];
  const defaultYear = storedYears[0] ? String(storedYears[0]) : years[0]!;
  const year = years.includes(chosenYear) ? chosenYear : defaultYear;
  const known = useMemo(
    () => [
      ...new Set([
        ...(catalog ?? []).map(item => item.contract),
        ...(coverage?.products ?? []).flatMap(item => item.contracts),
        ...(coverage?.contracts ?? []).map(item => item.contract),
      ]),
    ],
    [catalog, coverage],
  );
  const products = useMemo(
    () =>
      [
        ...new Set([
          ...knownProducts.filter(id => offeredExchanges.includes(id.split("/")[0]!)),
          ...(coverage?.products ?? []).map(item => item.product),
          ...known
            .map(id => id.split("/").slice(0, 2).join("/"))
            .filter(id => offeredExchanges.includes(id.split("/")[0]!)),
        ]),
      ].sort(),
    [offeredExchanges, coverage, known],
  );
  const rows = useMemo(() => {
    if (!acquires) return storedRows;
    const emptyRow = (id: string): Row => ({
      id,
      name: warehouseName(id),
      exchange: id.split("/")[0]!,
      months: new Map(),
    });
    if (!minutes) {
      const seen = new Set(storedRows.map(row => row.id));
      return [...storedRows, ...products.filter(id => !seen.has(id)).map(emptyRow)];
    }
    const targetYear = Number(year);
    const byId = new Map(storedRows.map(row => [row.id, row]));
    const inYear = (contract: string) => {
      const held = byId.get(contract);
      if (held && [...held.months.keys()].some(month => month.startsWith(`${year}-`))) return true;
      const [venue = "", , delivery = ""] = contract.split("/");
      const deliveryYear = Number(delivery.slice(0, 4));
      return (
        offeredExchanges.includes(venue) &&
        deliveryYear >= targetYear &&
        deliveryYear <= targetYear + 1
      );
    };
    const storedInYear = storedRows.filter(row => inYear(row.id));
    const seen = new Set(storedInYear.map(row => row.id));
    const unacquiredContracts = known
      .filter(id => !seen.has(id) && inYear(id))
      .sort()
      .map(emptyRow);
    const contractRows = [...storedInYear, ...unacquiredContracts];
    const coveredProducts = new Set(
      contractRows.map(row => row.id.split("/").slice(0, 2).join("/")),
    );
    return [...contractRows, ...products.filter(id => !coveredProducts.has(id)).map(emptyRow)];
    // warehouseName depends on the current locale.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [acquires, minutes, storedRows, products, known, offeredExchanges, year, locale]);
  const present = exchanges.filter(
    item =>
      (acquires && offeredExchanges.includes(item)) || rows.some(row => row.exchange === item),
  );
  const query = search.trim().toUpperCase();
  // The operator's products: collected, watched, backtested, run by a strategy or held.
  const saved = context.snapshot?.market_endpoint;
  const traded = Object.values(views).flatMap(view => [
    ...(view.positions?.rows ?? view.live?.positions ?? []).flatMap(
      position => position.instrument ?? [],
    ),
    ...(view.strategies?.instance?.instance?.instruments ?? []),
  ]);
  const tradedSeries = Object.values(views).flatMap(view =>
    (view.strategies?.instance?.instance?.series ?? []).map(
      item => `${item.exchange}/${item.product.toLowerCase()}`,
    ),
  );
  const mine = new Set([
    ...backtested.map(symbol => symbol.split("/").slice(0, 2).join("/")),
    ...tradedSeries,
    ...[...(saved && "instruments" in saved ? saved.instruments : []), ...traded].map(
      ({ exchange, symbol }) => `${exchange}/${symbol.replace(/\d.*$/, "").toLowerCase()}`,
    ),
  ]);
  const isMine = (row: Row) =>
    row.months.size > 0 || mine.has(row.id.split("/").slice(0, 2).join("/"));
  // An empty warehouse with nothing of the operator's shows everything there is to collect.
  const mineOnly = !everything && rows.some(isMine);
  const shownRows = rows.filter(
    row =>
      (!mineOnly || isMine(row)) &&
      (!exchange || row.exchange === exchange) &&
      (!query || `${row.id} ${row.name ?? ""}`.toUpperCase().includes(query)),
  );
  const matrixRows = minutes && !query ? shownRows.slice(0, 200) : shownRows;
  const matrixMonths = monthsOf(year);

  const error = state.error || options.error || (!viewed ? versionError : "");
  useErrorNotification(error, "asterion.terminal.data-workbench");
  if (!state.value || !options.value)
    return error ? (
      <WarehouseUnavailable
        error={error}
        retry={() => {
          void state.refresh();
          void options.refresh();
        }}
        openSettings={context.openSettings}
      />
    ) : (
      <p className="workspace-empty" role="status">
        {t("读取中")}
      </p>
    );

  const { summary } = state.value;
  const sharedPage =
    !viewed &&
    warehouse?.tasks !== undefined &&
    (!warehouse.nextTaskId || warehouse.tasks.length >= shown);
  const tasks = sharedPage ? warehouse.tasks! : state.value.tasks;
  const older = sharedPage ? warehouse.nextTaskId !== "" : state.value.older;
  const presetFor = (target: Table): Preset => {
    const item = summary.tables.find(entry => entry.table === target);
    const selectedExchange = exchange || present[0];
    const exchanges = selectedExchange ? [selectedExchange] : undefined;
    if (target === Table.MINUTES || !item?.firstDay || !item.missing)
      return exchanges ? { exchanges } : {};
    return {
      exchanges,
      first: item.firstDay,
      last: item.expectedLastDay || item.lastDay,
    };
  };

  return (
    <>
      <VersionBanner
        viewed={viewed}
        latest={latest}
        back={() => {
          setViewed("");
          context.navigate("workspace.data-dataset", {});
        }}
      />
      <div className="data-dataset">
        <div className="data-series-cards" role="group" aria-label={t("数据")}>
          {(["daily", "minutes"] as const).map(item => {
            const itemTable = item === "minutes" ? Table.MINUTES : Table.DAILY;
            const entry = summary.tables.find(row => row.table === itemTable);
            const status = freshness(entry);
            const targetCount =
              item === "minutes"
                ? (coverage?.contracts.length ?? 0)
                : (coverage?.products.length ?? 0);
            return (
              <button
                key={item}
                type="button"
                className="data-series-card"
                aria-pressed={kind === item}
                onClick={() => setKind(item)}
              >
                <span className="data-series-head">
                  <strong>{tableName(itemTable)}</strong>
                  {status && <StatusBadge tone={status.tone}>{status.text}</StatusBadge>}
                </span>
                <span className="data-series-figure">{entry?.lastDay || "—"}</span>
                {entry?.rows ? (
                  <span className="subtle">
                    {[
                      t("{day} 起", { day: entry.firstDay }),
                      targetCount > 0 &&
                        t(item === "minutes" ? "{count} 个合约" : "{count} 个品种", {
                          count: targetCount,
                        }),
                      t("{count} 行", { count: Number(entry.rows).toLocaleString(locale) }),
                    ]
                      .filter(Boolean)
                      .join(" · ")}
                  </span>
                ) : null}
                {entry?.sourceEmpty || (entry?.rows && entry.lastDay < entry.expectedLastDay) ? (
                  <span className="data-series-notes">
                    {entry.rows && entry.lastDay < entry.expectedLastDay ? (
                      <span>{t("应到 {day}", { day: entry.expectedLastDay })}</span>
                    ) : null}
                    {entry.sourceEmpty ? (
                      <span>
                        {t("另有 {count} 个交易日来源无数据", {
                          count: Number(entry.sourceEmpty),
                        })}
                      </span>
                    ) : null}
                  </span>
                ) : null}
              </button>
            );
          })}
        </div>
        <div className="data-coverage-body">
          <div className="data-toolbar">
            <search className="data-filters">
              <select
                aria-label={t("交易所")}
                value={exchange}
                onChange={event => setExchange(event.target.value)}
              >
                <option value="">{t("全部交易所")}</option>
                {present.map(item => (
                  <option key={item}>{item}</option>
                ))}
              </select>
              <input
                type="search"
                aria-label={minutes ? t("搜索合约") : t("搜索品种")}
                placeholder={minutes ? t("搜索合约") : t("搜索品种")}
                spellCheck={false}
                autoCorrect="off"
                autoCapitalize="none"
                value={search}
                onChange={event => setSearch(event.target.value)}
              />
              {acquires && (
                <label className="checkbox">
                  <input
                    type="checkbox"
                    checked={everything}
                    onChange={event => setEverything(event.target.checked)}
                  />
                  {t("全部品种")}
                </label>
              )}
              {years.length > 0 && (
                <select
                  aria-label={t("年份")}
                  value={year}
                  onChange={event => setYear(event.target.value)}
                >
                  {years.map(item => (
                    <option key={item}>{item}</option>
                  ))}
                </select>
              )}
            </search>
            <span className="data-legend" aria-hidden="true">
              {matrixRows.length > 0 &&
                legend
                  // Only a contract has a delivery month to pass.
                  .filter(([state]) => minutes || state !== "delisted")
                  .map(([state, label]) => (
                    <span key={state}>
                      <i data-cell={state} /> {t(label)}
                    </span>
                  ))}
            </span>
            {!viewed && (
              <div className="data-toolbar-actions">
                <Activity
                  tasks={tasks}
                  versions={versions?.items ?? []}
                  shown={shown}
                  more={older || versions?.older ? () => setShown(value => value + 20) : undefined}
                  refresh={refreshAll}
                  openVersion={version => {
                    setViewed(version);
                    context.navigate(
                      "workspace.data-dataset",
                      version ? { params: { version } } : {},
                    );
                  }}
                />
                {offered.length > 0 && (
                  <button type="button" onClick={() => void startUpdate()}>
                    {t("更新到最新")}
                  </button>
                )}
                {acquires && (
                  <button
                    type="button"
                    className="primary"
                    onClick={() => setAcquiring({ table, preset: presetFor(table) })}
                  >
                    {t("采集")}
                  </button>
                )}
              </div>
            )}
          </div>
          {updated && <p className="data-feedback">{updated}</p>}
          {!rows.length ? (
            <p className="data-empty subtle">{t(minutes ? "还没有分钟线" : "还没有日线")}</p>
          ) : !shownRows.length ? (
            <p className="data-empty subtle">{t("没有匹配的合约")}</p>
          ) : (
            <>
              <Matrix
                rows={matrixRows}
                months={matrixMonths}
                label={minutes ? t("分钟线覆盖") : t("日线覆盖")}
                heading={minutes ? t("合约") : t("品种")}
                onFill={
                  acquires
                    ? (id, month, state) =>
                        setAcquiring({
                          table,
                          preset: minutes
                            ? { instruments: [id], ...monthRange(month), refetch: false }
                            : {
                                exchanges: [id.split("/")[0]!],
                                ...monthRange(month),
                                // Daily days are stored per exchange: one holding other
                                // products is skipped unless refetched.
                                refetch: state === "partial" || state === "missing",
                              },
                        })
                    : undefined
                }
              />
              {minutes && !query && shownRows.length > 200 && (
                <p className="data-coverage-note subtle">
                  {t("另有 {count} 个合约，搜索以缩小范围", { count: shownRows.length - 200 })}
                </p>
              )}
            </>
          )}
        </div>
        {acquiring && (
          <Acquisition
            table={acquiring.table}
            name={tableName(acquiring.table)}
            options={options.value}
            products={products}
            known={known}
            preset={acquiring.preset}
            close={() => setAcquiring(undefined)}
            started={() => {
              setAcquiring(undefined);
              void refreshAll();
            }}
          />
        )}
      </div>
    </>
  );
}

function Matrix({
  rows,
  months,
  label,
  heading,
  onFill,
}: {
  rows: Row[];
  months: string[];
  label: string;
  heading: string;
  onFill?: (id: string, month: string, state: Cell) => void;
}) {
  const latestMonth = beijingDay(-1).slice(0, 7);
  const groups = exchanges
    .map(exchange => ({ exchange, rows: rows.filter(row => row.exchange === exchange) }))
    .filter(group => group.rows.length);
  // Set directly: a native title waits for the pointer to settle and often skips
  // neighbours with the same text, and a state update would redraw the whole grid.
  const tip = useRef<HTMLDivElement>(null);
  const tipped = useRef<Element | null>(null);
  const showTip = (cell: Element | null) => {
    tipped.current = cell;
    const box = tip.current!;
    box.textContent = cell?.getAttribute("aria-description") ?? "";
    if (!cell) return;
    const at = cell.getBoundingClientRect();
    const { width, height } = box.getBoundingClientRect();
    box.style.left = `${Math.max(8, Math.min(at.left + (at.width - width) / 2, innerWidth - width - 8))}px`;
    box.style.top = `${at.top - height - 6 < 8 ? at.bottom + 6 : at.top - height - 6}px`;
  };
  // A poll can change the month under the pointer or remove its row.
  useEffect(() => {
    if (tipped.current) showTip(tipped.current.isConnected ? tipped.current : null);
  });
  return (
    <div
      className="coverage-matrix"
      role="region"
      aria-label={label}
      tabIndex={0}
      onPointerOver={event => {
        // The gaps between cells keep the current tip.
        const cell = (event.target as Element).closest("td, th");
        if (cell) showTip(cell.matches("td") ? cell : null);
      }}
      onPointerLeave={() => showTip(null)}
      onScroll={() => showTip(tipped.current?.matches(":hover") ? tipped.current : null)}
      onFocus={event => {
        const cell = event.target.closest("td");
        if (cell && event.target.matches(":focus-visible")) showTip(cell);
      }}
      onBlur={() => {
        if (!tipped.current?.matches(":hover")) showTip(null);
      }}
    >
      <div ref={tip} className="coverage-tip" role="tooltip" />
      <table className="data-table" aria-label={label}>
        <thead>
          <tr>
            <th scope="col">{heading}</th>
            {months.map(month => (
              <th scope="col" key={month}>
                {month.slice(5)}
              </th>
            ))}
          </tr>
        </thead>
        {groups.map(group => (
          <tbody key={group.exchange}>
            <tr className="coverage-group">
              <th colSpan={months.length + 1}>{group.exchange}</th>
            </tr>
            {group.rows.map(row => {
              const held = [...row.months.keys()].sort();
              return (
                <tr key={row.id}>
                  <th scope="row">
                    <span className="coverage-id">{row.id}</span>
                    {row.name && <small className="coverage-name">{row.name}</small>}
                  </th>
                  {months.map(month => {
                    const cell = row.months.get(month);
                    const state = cellOf(row, month, held[0], held.at(-1), latestMonth);
                    const fillable =
                      !!onFill &&
                      (state === "partial" ||
                        state === "missing" ||
                        state === "pending" ||
                        state === "unacquired");
                    const text = cellText(cell, state, fillable);
                    const action = t("补采 {target} {month}", { target: row.id, month });
                    return (
                      <td key={month} data-cell={state} aria-description={cell && describe(cell)}>
                        {fillable ? (
                          <button
                            type="button"
                            aria-label={text && text !== "+" ? `${action} (${text})` : action}
                            onClick={() => onFill(row.id, month, state)}
                          >
                            {text === "+" ? <span aria-hidden="true">+</span> : text}
                          </button>
                        ) : (
                          text
                        )}
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        ))}
      </table>
    </div>
  );
}
