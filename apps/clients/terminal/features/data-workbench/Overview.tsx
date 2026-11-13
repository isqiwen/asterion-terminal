import { useEffect, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import type {
  AcquisitionTask,
  DatasetChange,
  Table,
} from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarket } from "../../src/host/market";
import { getLocale } from "../../src/i18n";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { Acquisition } from "./Acquisition";
import { Activity, activeTask } from "./Activity";
import { WarehousePage } from "./Page";
import { freshness, t, tableName, tableOrder, usePolled } from "./dataset";

// What the dataset holds, whether it is up to date, and what is happening to it.
export function Overview(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据概览")}>
      {() => <OverviewBody context={context} />}
    </WarehousePage>
  );
}

function OverviewBody({ context }: { context: TerminalContext }) {
  // Activity rows to show: one page, more on request.
  const [shown, setShown] = useState(20);
  const [acquiring, setAcquiring] = useState<Table>();
  const catalog = useMarket()?.board.catalog?.contracts;
  const options = usePolled(() => warehouseData({ case: "getAcquisitionOptions", value: {} }), []);
  const state = usePolled(async () => {
    const summary = await warehouseData({ case: "getDatasetSummary", value: {} });
    // Warehouse lists tasks newest first; read as many pages as shown.
    const tasks: AcquisitionTask[] = [];
    let afterId = "";
    do {
      const page = await warehouseData({ case: "listAcquisitionTasks", value: { afterId } });
      tasks.push(...page.tasks);
      afterId = page.nextId;
    } while (afterId && tasks.length < shown);
    return { summary, tasks, older: afterId !== "" };
  }, [shown]);
  const latest = state.value?.summary.dataset?.version ?? "";
  // Versions change only when a task publishes one.
  const [versions, setVersions] = useState<{ items: DatasetChange[]; older: boolean }>();
  const [versionError, setVersionError] = useState<DisplayError>("");
  useEffect(() => {
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
  }, [latest, shown]);
  const update = useActionError();
  const [updated, setUpdated] = useState("");
  const startUpdate = () =>
    update.attempt(async () => {
      setUpdated("");
      const reply = await warehouseData({ case: "startUpdate", value: {} });
      setUpdated(
        reply.tasks.length
          ? t("已提交 {count} 个更新任务", { count: reply.tasks.length })
          : t("各表已是最新，或已有任务在进行"),
      );
      await state.refresh();
    });
  const error = state.error || options.error || versionError;
  if (!state.value || !options.value)
    return error ? (
      <div className="workspace-empty" role="alert">
        <strong>{t("读取历史数据失败")}</strong>
        <ErrorNotice error={error} />
      </div>
    ) : (
      <p className="workspace-empty" role="status">
        {t("读取中")}
      </p>
    );
  const { summary, tasks, older } = state.value;
  const running = tasks.filter(activeTask).length;
  const offered = options.value.tables;
  return (
    <div className="data-overview">
      <ErrorNotice error={error} />
      <header className="data-overview-header">
        <div>
          <h3>{t("研究数据集")}</h3>
          <span className="subtle">
            {latest ? (
              <>
                <code title={latest}>{latest.slice(0, 8)}</code>
                {" · "}
                {new Date(Number(summary.dataset!.createdMs)).toLocaleString(getLocale(), {
                  hour12: false,
                })}
              </>
            ) : (
              t("还没有数据")
            )}
          </span>
        </div>
        {running > 0 && (
          <StatusBadge tone="neutral">{t("{count} 个任务进行中", { count: running })}</StatusBadge>
        )}
        {offered.length > 0 && (
          <button type="button" className="primary" onClick={() => void startUpdate()}>
            {t("更新到最新")}
          </button>
        )}
      </header>
      {updated && <p className="subtle">{updated}</p>}
      <ActionError error={update.error} />
      <table className="data-table data-status" aria-label={t("数据状态")}>
        <thead>
          <tr>
            <th>{t("数据")}</th>
            <th>{t("区间")}</th>
            <th className="numeric">{t("行数")}</th>
            <th>{t("状态")}</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {tableOrder.map(table => {
            const item = summary.tables.find(entry => entry.table === table);
            const status = freshness(item);
            return (
              <tr key={table}>
                <th scope="row">{tableName(table)}</th>
                <td>{item?.firstDay ? `${item.firstDay} – ${item.lastDay}` : "—"}</td>
                <td className="numeric">
                  {item?.rows ? Number(item.rows).toLocaleString(getLocale()) : "—"}
                </td>
                <td>
                  {status && <StatusBadge tone={status.tone}>{status.text}</StatusBadge>}
                  {item?.rows && item.lastDay < item.expectedLastDay ? (
                    <span className="subtle">
                      {" "}
                      {t("应到 {day}", { day: item.expectedLastDay })}
                    </span>
                  ) : null}
                </td>
                <td>
                  {offered.includes(table) && (
                    <button type="button" onClick={() => setAcquiring(table)}>
                      {t("采集")}
                    </button>
                  )}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <Activity
        tasks={tasks}
        versions={versions?.items ?? []}
        shown={shown}
        more={older || versions?.older ? () => setShown(value => value + 20) : undefined}
        refresh={state.refresh}
        openVersion={version =>
          context.navigate("workspace.data-coverage", version ? { params: { version } } : {})
        }
      />
      {acquiring !== undefined && (
        <Acquisition
          table={acquiring}
          name={tableName(acquiring)}
          options={options.value}
          known={(catalog ?? []).map(item => item.contract)}
          preset={{}}
          close={() => setAcquiring(undefined)}
          started={() => {
            setAcquiring(undefined);
            void state.refresh();
          }}
        />
      )}
    </div>
  );
}
