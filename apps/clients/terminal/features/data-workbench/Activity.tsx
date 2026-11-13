import { warehouseData } from "../../src/bridge/warehouse-data";
import {
  TaskState,
  type AcquisitionTask,
  type DatasetChange,
} from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { getLocale, listSeparator } from "../../src/i18n";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { t, tableName } from "./dataset";
import { changeText } from "./Page";

const states: Record<number, string> = {
  [TaskState.QUEUED]: "等待采集",
  [TaskState.RUNNING]: "采集中",
  [TaskState.SUCCEEDED]: "已完成",
  [TaskState.FAILED]: "失败",
  [TaskState.CANCELLED]: "已取消",
  [TaskState.INTERRUPTED]: "已中断",
};
const tones: Record<number, Tone> = {
  [TaskState.SUCCEEDED]: "ok",
  [TaskState.FAILED]: "danger",
  [TaskState.CANCELLED]: "attention",
  [TaskState.INTERRUPTED]: "attention",
};
export const activeTask = (task: AcquisitionTask) =>
  [TaskState.QUEUED, TaskState.RUNNING].includes(task.state);
const time = (ms: bigint) =>
  new Date(Number(ms)).toLocaleString(getLocale(), {
    hour12: false,
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });

// Every client's tasks and the versions they published, newest first.
export function Activity({
  tasks,
  versions,
  shown,
  more,
  refresh,
  openVersion,
}: {
  tasks: AcquisitionTask[];
  // Newest first: the first is the current version.
  versions: DatasetChange[];
  shown: number;
  more?: () => void;
  refresh: () => Promise<void>;
  openVersion: (version: string) => void;
}) {
  const cancelled = useActionError();
  const cancel = (id: string) =>
    cancelled.attempt(async () => {
      await warehouseData({ case: "cancelAcquisition", value: { id } });
      await refresh();
    });
  const current = versions[0]?.dataset?.version;
  const items = [
    ...tasks.map(task => ({ at: task.startedMs, task })),
    ...versions.map(version => ({ at: version.dataset!.createdMs, version })),
  ]
    .sort((a, b) => Number(b.at - a.at))
    .slice(0, shown);
  return (
    <section className="data-activity" aria-label={t("最近活动")}>
      <h3>{t("最近活动")}</h3>
      <ActionError error={cancelled.error} />
      {items.length ? (
        <table className="data-table" aria-label={t("最近活动")}>
          <tbody>
            {items.map(item =>
              "version" in item ? (
                <VersionRow
                  key={item.version.dataset!.version}
                  version={item.version}
                  current={item.version.dataset!.version === current}
                  open={openVersion}
                />
              ) : (
                <TaskRow key={item.task.request!.id} task={item.task} cancel={cancel} />
              ),
            )}
          </tbody>
        </table>
      ) : (
        <p className="subtle">{t("还没有采集任务。")}</p>
      )}
      {more && (
        <button type="button" onClick={more}>
          {t("更早的活动")}
        </button>
      )}
    </section>
  );
}

function VersionRow({
  version,
  current,
  open,
}: {
  version: DatasetChange;
  current: boolean;
  open: (version: string) => void;
}) {
  const id = version.dataset!.version;
  return (
    <tr data-kind="version">
      <td className="data-time">{time(version.dataset!.createdMs)}</td>
      <td>
        {t("发布版本")} <code title={id}>{id.slice(0, 8)}</code>
      </td>
      <td>{current && <StatusBadge tone="ok">{t("当前")}</StatusBadge>}</td>
      <td className="subtle">{changeText(version)}</td>
      <td>
        <button type="button" onClick={() => open(current ? "" : id)}>
          {t("查看")}
        </button>
      </td>
    </tr>
  );
}

function TaskRow({ task, cancel }: { task: AcquisitionTask; cancel: (id: string) => unknown }) {
  const request = task.request!;
  return (
    <tr
      title={t("任务 {id} · 请求 {count} 次", {
        id: request.id,
        count: task.requestsSent.toString(),
      })}
    >
      <td className="data-time">{time(task.startedMs)}</td>
      <td>
        {request.id.startsWith("update-") ? t("更新") : t("采集")} {tableName(request.table)}
        <span className="subtle">
          {" · "}
          {[...request.exchanges, ...request.instruments].join(listSeparator()) || t("全部交易所")}
          {" · "}
          {request.firstDay || t("上市日")} – {request.lastDay || t("摘牌日")}
        </span>
        {task.error && <div className="task-error">{task.error.message}</div>}
      </td>
      <td>
        <StatusBadge tone={tones[task.state] ?? "neutral"}>
          {t(states[task.state] ?? String(task.state))}
        </StatusBadge>
      </td>
      <td className="subtle">
        {task.itemsDone.toString()} / {task.items.toString()}
        {task.itemsEmpty > 0n &&
          ` · ${t("{count} 天尚无数据", { count: Number(task.itemsEmpty) })}`}
      </td>
      <td>
        {activeTask(task) && (
          <button type="button" onClick={() => void cancel(request.id)}>
            {t("取消")}
          </button>
        )}
      </td>
    </tr>
  );
}
