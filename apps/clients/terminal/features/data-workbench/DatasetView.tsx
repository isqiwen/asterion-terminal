import { useEffect, useMemo, useRef, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import {
  Table,
  type AcquisitionTask,
  type ContractCoverage,
  type DatasetChange,
  type DatasetCoverage,
} from "../../src/generated/asterion/warehouse_pb";
import { useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useErrorNotification } from "../../src/host/notifications";
import { useWorkspaceActive } from "../../src/host/polling";
import { automaticTask, useWarehouse, useWarehousePoll } from "../../src/host/warehouse";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { getLocale } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { warehouseName } from "../../src/ui/contract-names";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { ContractHistory } from "../futures-market/ContractHistory";
import { Activity, activeTask } from "./Activity";
import {
  contractCode,
  CoverageList,
  productRules,
  type Kind,
  type ProductRow,
  type Rule,
} from "./CoverageList";
import { VersionBanner, WarehousePage, WarehouseUnavailable } from "./Page";
import { exchanges, startAcquisition, t, tableName } from "./dataset";

// What the research dataset holds for daily and minute bars, where any gaps sit
// across products and months, and the acquisitions and versions behind it.
export function Dataset(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("采集")}>
      {() => <DatasetBody context={context} />}
    </WarehousePage>
  );
}

function DatasetBody({ context }: { context: TerminalContext }) {
  const paramVersion = context.workspaceParams?.version ?? "";
  const [viewed, setViewed] = useState(paramVersion);
  const [kind, setKind] = useWorkspaceDraft<Kind>("coverage-kind", "daily");
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
  const [openProduct, setOpenProduct] = useWorkspaceDraft("coverage-open", "");
  const workspaceActive = useWorkspaceActive();
  const [chart, setChart] = useState("");
  useEffect(() => {
    if (!chart || !workspaceActive) return;
    const close = (event: KeyboardEvent) => {
      // Escape belongs to a dialog or popover open above the chart.
      if (event.key === "Escape" && !document.querySelector("dialog[open], :popover-open"))
        setChart("");
    };
    document.addEventListener("keydown", close);
    return () => document.removeEventListener("keydown", close);
  }, [chart, workspaceActive]);
  const [shown, setShown] = useState(20);
  const [issuesOnly, setIssuesOnly] = useState(false);
  const warehouse = useWarehouse();
  const warehouseRef = useRef(warehouse);
  warehouseRef.current = warehouse;
  const options = useWarehousePoll(
    () => warehouseData({ case: "getAcquisitionOptions", value: {} }),
    [],
    true,
  );
  const loadedCoverage = useRef<{ key: string; coverage?: DatasetCoverage }>({ key: "" });
  const state = useWarehousePoll(async () => {
    const current = await warehouseData({ case: "getDatasetSummary", value: {} });
    const latest = current.dataset?.version ?? "";
    const version = viewed || latest;
    const summary =
      viewed && viewed !== latest
        ? await warehouseData({ case: "getDatasetSummary", value: { version: viewed } })
        : current;
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
    // Recounted when the version moves or a task ends, which can record days as empty.
    const active = (useSharedTasks ? shared.tasks! : tasks).filter(activeTask);
    const coverageKey = `${version}/${active.map(item => item.request!.id).join()}`;
    if (coverageKey !== loadedCoverage.current.key)
      loadedCoverage.current = {
        key: coverageKey,
        coverage: version
          ? await warehouseData({ case: "getDatasetCoverage", value: { version } })
          : undefined,
      };
    const { coverage } = loadedCoverage.current;
    return { latest, version, summary, coverage, tasks, older: afterId !== "" };
  }, [viewed, shown]);
  const refreshAll = async () => {
    await warehouse?.refresh();
    await state.refresh();
  };
  const hasSummary = state.value !== undefined;
  const latest = state.value?.latest ?? "";
  const [versions, setVersions] = useState<{ items: DatasetChange[]; older: boolean }>();
  const [versionError, setVersionError] = useState<DisplayError>("");
  useEffect(() => {
    if (viewed || !hasSummary) return;
    if (!latest) {
      setVersions({ items: [], older: false });
      return;
    }
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
  }, [hasSummary, latest, shown, viewed]);

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
      const submitted = reply.tasks.filter(task => !automaticTask(task)).length;
      setUpdated(
        submitted
          ? t("已提交 {count} 个更新任务", { count: submitted })
          : hasGaps && !hasActive
            ? t("各表末日已到最新；中间缺口请点击对应缺口或“采集”补齐")
            : t("各表已是最新，或已有任务在进行"),
      );
      await refreshAll();
    });

  const filling = useRef(new Set<string>());
  const start = (rules: Rule[]) => {
    const key = JSON.stringify(rules);
    if (filling.current.has(key)) return;
    filling.current.add(key);
    void update
      .attempt(async () => {
        for (const rule of rules) await startAcquisition(rule);
        await refreshAll();
      })
      .finally(() => filling.current.delete(key));
  };
  const coverage = state.value?.coverage;
  const version = state.value?.version ?? "";
  const table = kind === "minutes" ? Table.MINUTES : Table.DAILY;
  const offered = options.value?.tables.map(item => item.table) ?? [];
  const acquires = !viewed && offered.includes(table);
  const locale = getLocale();
  const rows = useMemo<ProductRow[]>(
    () =>
      (coverage?.products ?? [])
        .map(item => ({
          id: item.product,
          name: warehouseName(item.product) ?? "",
          exchange: item.product.split("/")[0]!,
          coverage: item,
        }))
        .sort((a, b) => a.id.localeCompare(b.id)),
    // warehouseName depends on the current locale.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [coverage, locale],
  );
  const present = useMemo(
    () => exchanges.filter(item => rows.some(row => row.exchange === item)),
    [rows],
  );
  const query = search.trim().toUpperCase();
  // A contract code such as RB2705 finds its product.
  const searchedProduct = /^([A-Z]{1,3})\d{3,4}$/.exec(query)?.[1];
  const { issueCount, shownRows } = useMemo(() => {
    const needsWork = ({ coverage: item }: ProductRow) =>
      !!(item?.[kind]?.withGaps || item?.[kind]?.pending);
    const filtered = rows.filter(
      row =>
        (!exchange || row.exchange === exchange) &&
        (!query ||
          `${row.id} ${row.name}`.toUpperCase().includes(query) ||
          row.id.split("/")[1]!.toUpperCase() === searchedProduct),
    );
    const issues = filtered.filter(needsWork);
    return {
      issueCount: issues.length,
      shownRows: issuesOnly ? issues : filtered,
    };
  }, [rows, exchange, query, searchedProduct, kind, issuesOnly]);
  const expanded =
    (searchedProduct &&
      shownRows.find(row => row.id.split("/")[1]!.toUpperCase() === searchedProduct)?.id) ||
    openProduct;
  const detailKey = expanded && version ? `${version}/${expanded}` : "";
  const [detail, setDetail] = useState<{ key: string; contracts: ContractCoverage[] }>();
  const [detailError, setDetailError] = useState<DisplayError>("");
  useErrorNotification(detailError, "asterion.terminal.data-workbench");
  useEffect(() => {
    if (!detailKey) return;
    let active = true;
    setDetailError("");
    warehouseData({ case: "getProductCoverage", value: { version, product: expanded } })
      .then(reply => active && setDetail({ key: detailKey, contracts: reply.contracts }))
      .catch(reason => active && setDetailError(asDisplayError(reason)));
    return () => {
      active = false;
    };
    // A new coverage reading means the open product's contracts changed too.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [detailKey, coverage]);
  const contracts = detail?.key === detailKey ? detail.contracts : undefined;
  // A product never acquired starts with its listed contracts' whole lives.
  const collect = (row: ProductRow) =>
    void update.attempt(async () => {
      const { contracts: held } = await warehouseData({
        case: "getProductCoverage",
        value: { version, product: row.id },
      });
      start(productRules(held, kind));
    });

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
  const running = tasks.filter(activeTask);
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
        <div className="data-summary" role="group" aria-label={t("数据")}>
          {(["daily", "minutes"] as const).map(item => {
            const itemTable = item === "minutes" ? Table.MINUTES : Table.DAILY;
            const entry = summary.tables.find(row => row.table === itemTable);
            const detail = entry?.rows
              ? [
                  t("数据到 {day}", { day: entry.lastDay }),
                  entry.lastDay < entry.expectedLastDay &&
                    t("应到 {day}", { day: entry.expectedLastDay }),
                  entry.sourceEmpty > 0n &&
                    t("另有 {count} 个交易日来源无数据", { count: Number(entry.sourceEmpty) }),
                ]
                  .filter(Boolean)
                  .join(" · ")
              : undefined;
            return (
              <button
                key={item}
                type="button"
                className="data-summary-item"
                title={detail}
                aria-pressed={kind === item}
                onClick={() => setKind(item)}
              >
                <strong>{tableName(itemTable)}</strong>
                {!!entry?.missing && (
                  <StatusBadge tone="attention">
                    {t("缺 {count} 个交易日", { count: Number(entry.missing) })}
                  </StatusBadge>
                )}
              </button>
            );
          })}
          <span className="data-legend" aria-hidden="true">
            {(
              [
                ["held", "已采"],
                ["part", "部分采集"],
                ["gap", "缺口"],
                ["pending", "待更新"],
                ["empty", "来源无数据"],
                ["none", "未采集"],
                ["future", "未到期"],
              ] as const
            ).map(([part, label]) => (
              <span key={part}>
                <i data-part={part} /> {t(label)}
              </span>
            ))}
          </span>
        </div>
        <div className="data-coverage-body">
          <div className="data-toolbar">
            <search className="data-filters">
              <input
                type="search"
                aria-label={t("搜索品种或合约")}
                placeholder={t("搜索品种或合约，如 rb2705")}
                spellCheck={false}
                autoCorrect="off"
                autoCapitalize="none"
                value={search}
                onChange={event => setSearch(event.target.value)}
              />
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
              <button
                type="button"
                aria-pressed={issuesOnly}
                onClick={() => setIssuesOnly(!issuesOnly)}
              >
                {t("需处理 {count}", { count: issueCount })}
              </button>
            </search>
            {!viewed && (
              <div className="data-toolbar-actions">
                <Activity
                  tasks={tasks}
                  versions={versions?.items ?? []}
                  shown={shown}
                  more={older || versions?.older ? () => setShown(value => value + 20) : undefined}
                  refresh={refreshAll}
                  openVersion={item => {
                    setViewed(item);
                    context.navigate(
                      "workspace.data-dataset",
                      item ? { params: { version: item } } : {},
                    );
                  }}
                />
                {offered.length > 0 && (
                  <button type="button" onClick={() => void startUpdate()}>
                    {t("更新到最新")}
                  </button>
                )}
              </div>
            )}
          </div>
          {updated && (
            <p className="data-feedback" role="status">
              {updated}
            </p>
          )}
          <div className="data-coverage-main">
            {!rows.length ? (
              <p className="data-empty subtle">{t("暂无品种")}</p>
            ) : !shownRows.length ? (
              <p className="data-empty subtle">{t("没有匹配的品种或合约")}</p>
            ) : (
              <CoverageList
                kind={kind}
                rows={shownRows}
                contracts={contracts}
                open={expanded}
                setOpen={setOpenProduct}
                query={query}
                running={running}
                acquires={acquires}
                start={start}
                collect={collect}
                viewing={chart}
                view={item => setChart(item === chart ? "" : item)}
              />
            )}
            {chart && (
              <aside className="coverage-drawer" aria-label={contractCode(chart)}>
                <header>
                  <strong>{contractCode(chart)}</strong>
                  <span>{warehouseName(chart)}</span>
                  <button type="button" onClick={() => setChart("")}>
                    {t("关闭")}
                  </button>
                </header>
                <div className="coverage-drawer-body">
                  <ContractHistory
                    key={chart}
                    venue={chart.split("/")[0]!}
                    symbol={chart}
                    historyContractId={chart}
                    datasetVersion={version}
                    context={context}
                  />
                </div>
              </aside>
            )}
          </div>
        </div>
      </div>
    </>
  );
}
