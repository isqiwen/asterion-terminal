import { useId, useRef, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import {
  Table,
  TaskState,
  type AcquisitionTask,
  type DatasetChange,
} from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { automaticTable, automaticTask } from "../../src/host/warehouse";
import { ErrorNotice, remoteError } from "../../src/i18n/errors";
import { listSeparator } from "../../src/i18n";
import { Icon } from "../../src/ui/Icon";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { Tag } from "../../src/ui/Tag";
import { t, tableName } from "./dataset";

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
const primaryRank = [
  Table.MINUTES,
  Table.DAILY,
  Table.WAREHOUSE_RECEIPTS,
  Table.MEMBER_POSITIONS,
  Table.INVENTORY,
  Table.SETTLEMENT,
  Table.PRICE_TICKS,
  Table.CONTRACTS,
  Table.CALENDAR,
];
const pendingTask = (task: AcquisitionTask) =>
  [TaskState.QUEUED, TaskState.RUNNING].includes(task.state);
export const activeTask = (task: AcquisitionTask) => !automaticTask(task) && pendingTask(task);
const batchKey = (id: string) =>
  /^(acq-[A-Za-z0-9]+|update-\d+)-(?:daily|minutes|settlement|price_ticks|contracts|calendar|warehouse_receipts|member_positions|inventory)$/.exec(
    id,
  )?.[1] ?? id;

const backgroundKey = "background";

export type TaskBatch = {
  key: string;
  members: AcquisitionTask[];
  primary: AcquisitionTask;
  startedMs: bigint;
  state: TaskState;
  activeIds: string[];
  itemsDone: bigint;
  items: bigint;
  requestsSent: bigint;
};

export function groupTasks(tasks: readonly AcquisitionTask[]): TaskBatch[] {
  const byBatch = new Map<string, AcquisitionTask[]>();
  for (const task of tasks) {
    // Automatic work is one row while it runs and leaves no record unless it stops early.
    if (automaticTask(task) && task.state === TaskState.SUCCEEDED) continue;
    const key =
      automaticTask(task) && pendingTask(task) ? backgroundKey : batchKey(task.request!.id);
    const list = byBatch.get(key);
    if (list) list.push(task);
    else byBatch.set(key, [task]);
  }
  return [...byBatch.entries()].map(([key, members]) => {
    const primary =
      (key === backgroundKey && members.find(item => item.state === TaskState.RUNNING)) ||
      [...members].sort(
        (a, b) => primaryRank.indexOf(a.request!.table) - primaryRank.indexOf(b.request!.table),
      )[0]!;
    const state = members.some(item => item.state === TaskState.RUNNING)
      ? TaskState.RUNNING
      : members.some(item => item.state === TaskState.QUEUED)
        ? TaskState.QUEUED
        : members.some(item => item.state === TaskState.FAILED)
          ? TaskState.FAILED
          : members.some(item => item.state === TaskState.INTERRUPTED)
            ? TaskState.INTERRUPTED
            : members.some(item => item.state === TaskState.CANCELLED)
              ? TaskState.CANCELLED
              : TaskState.SUCCEEDED;
    return {
      key,
      members,
      primary,
      startedMs: members.reduce((max, item) => (item.startedMs > max ? item.startedMs : max), 0n),
      state,
      activeIds: members.filter(pendingTask).map(item => item.request!.id),
      itemsDone: members.reduce((sum, item) => sum + item.itemsDone, 0n),
      items: members.reduce((sum, item) => sum + item.items, 0n),
      requestsSent: members.reduce((sum, item) => sum + item.requestsSent, 0n),
    };
  });
}

const time = (ms: bigint) =>
  new Date(Number(ms)).toLocaleString("sv-SE", {
    timeZone: "Asia/Shanghai",
    hour12: false,
  });
const versionDelta = (change: DatasetChange) => {
  if (!change.replaced && !change.removed) {
    return change.added ? t("新增 {count} 个分区", { count: change.added }) : t("无分区变更");
  }
  return t("分区：新增 {added} · 替换 {replaced} · 删除 {removed}", {
    added: change.added,
    replaced: change.replaced,
    removed: change.removed,
  });
};

// Every client's tasks and the versions they published, newest first, opened on demand.
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
  const panelId = useId();
  const anchor = `--data-activity-${panelId.replace(/:/g, "")}`;
  const trigger = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLElement>(null);
  const [open, setOpen] = useState(false);
  const cancelled = useActionError();
  const cancel = (ids: readonly string[]) =>
    cancelled.attempt(async () => {
      for (const id of ids) await warehouseData({ case: "cancelAcquisition", value: { id } });
      await refresh();
    });
  const current = versions[0]?.dataset?.version;
  const batches = groupTasks(tasks);
  const running = batches.filter(batch => batch.activeIds.length > 0).length;
  // A version made only by automatic work is not news; it stays readable by its id.
  const shownVersions = versions.filter(
    version => !version.tables.length || !version.tables.every(automaticTable),
  );
  const items = [
    ...batches.map(batch => ({ at: batch.startedMs, batch })),
    ...shownVersions.map(version => ({ at: version.dataset!.createdMs, version })),
  ]
    .sort((a, b) => Number(b.at - a.at))
    .slice(0, shown);
  return (
    <div className="data-activity-center">
      <button
        ref={trigger}
        type="button"
        className="data-activity-trigger"
        style={{ anchorName: anchor }}
        {...{ commandfor: panelId, command: "toggle-popover" }}
        aria-expanded={open}
      >
        {t("最近活动")}
        {running > 0 && (
          <StatusBadge tone="attention">
            {t("{count} 个任务进行中", { count: running })}
          </StatusBadge>
        )}
      </button>
      <section
        ref={panel}
        id={panelId}
        popover="auto"
        className="data-activity-popover"
        aria-label={t("最近活动")}
        style={{ positionAnchor: anchor }}
        onToggle={event => setOpen(event.newState === "open")}
      >
        <header className="data-activity-header">
          <h2>{t("最近活动")}</h2>
          <button
            type="button"
            className="data-activity-close"
            aria-label={t("关闭")}
            {...{ commandfor: panelId, command: "hide-popover" }}
            onClick={() => trigger.current?.focus()}
          >
            <Icon name="close" size={14} />
          </button>
        </header>
        <ActionError error={cancelled.error} namespace="asterion.terminal.data-workbench" />
        {open &&
          (items.length ? (
            <div className="data-activity-scroll">
              <table className="data-table data-activity-table" aria-label={t("最近活动")}>
                <colgroup>
                  <col className="col-act-time" />
                  <col className="col-act-subject" />
                  <col className="col-act-state" />
                  <col className="col-act-metric" />
                  <col className="col-act-action" />
                </colgroup>
                <tbody>
                  {items.map(item =>
                    "version" in item ? (
                      <VersionRow
                        key={item.version.dataset!.version}
                        version={item.version}
                        current={item.version.dataset!.version === current}
                        open={version => {
                          panel.current?.hidePopover?.();
                          openVersion(version);
                        }}
                      />
                    ) : (
                      <TaskRow key={item.batch.key} batch={item.batch} cancel={cancel} />
                    ),
                  )}
                </tbody>
              </table>
            </div>
          ) : (
            <p className="data-empty subtle">{t("还没有采集任务")}</p>
          ))}
        {open && more && (
          <div className="data-panel-footer">
            <button type="button" className="ghost-action" onClick={more}>
              {t("更早的活动")}
            </button>
          </div>
        )}
      </section>
    </div>
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
    <tr data-kind="version" data-current={current || undefined}>
      <td className="data-time">{time(version.dataset!.createdMs)}</td>
      <td className="data-subject">
        <span className="data-version-label">{t("发布版本")}</span>{" "}
        <code className="data-version-pill" title={id}>
          {id.slice(0, 8)}
        </code>
      </td>
      <td className="data-state">{current && <Tag>{t("当前")}</Tag>}</td>
      <td className="data-metric subtle">{versionDelta(version)}</td>
      <td className="data-row-action">
        <button type="button" className="ghost-action" onClick={() => open(current ? "" : id)}>
          {t("查看")}
        </button>
      </td>
    </tr>
  );
}

function TaskRow({
  batch,
  cancel,
}: {
  batch: TaskBatch;
  cancel: (ids: readonly string[]) => unknown;
}) {
  const request = batch.primary.request!;
  const running = batch.activeIds.length > 0;
  const weekly = batch.members.filter(item => item.request!.table === Table.INVENTORY);
  const weeklyEmpty = weekly.reduce((sum, item) => sum + item.itemsEmpty, 0n);
  const dailyEmpty = batch.members.reduce((sum, item) => sum + item.itemsEmpty, 0n) - weeklyEmpty;
  const weeklyNote =
    weeklyEmpty === 0n
      ? ""
      : weeklyEmpty === weekly.reduce((sum, item) => sum + item.items, 0n)
        ? t("没有新周报")
        : t("周报按报告日提供");
  const targets =
    [
      ...new Set(
        batch.members.flatMap(item => [...item.request!.exchanges, ...item.request!.instruments]),
      ),
    ].join(listSeparator()) || t("全部交易所");
  const firstDays = batch.members
    .map(item => item.request!.firstDay)
    .filter(Boolean)
    .sort();
  const lastDays = batch.members
    .map(item => item.request!.lastDay)
    .filter(Boolean)
    .sort();
  const span = `${firstDays[0] || t("上市日")} – ${lastDays.at(-1) || t("摘牌日")}`;
  const isUpdate = batch.key.startsWith("update-");
  const title =
    batch.key === backgroundKey
      ? `${t("后台更新")} · ${tableName(request.table)}`
      : isUpdate
        ? batch.members.length > 1
          ? t("更新到最新")
          : `${t("更新")} ${tableName(request.table)}`
        : `${t("采集")} ${tableName(request.table)}`;
  return (
    <tr
      data-kind="task"
      title={`${targets} · ${span} · ${t("任务 {id} · 请求 {count} 次", {
        id: batch.key,
        count: batch.requestsSent.toString(),
      })}`}
    >
      <td className="data-time">{time(batch.startedMs)}</td>
      <td className="data-subject">
        <div className="data-subject-line">
          <span className="data-task-title">{title}</span>
          <span className="data-task-targets">
            {" · "}
            {targets}
          </span>
          <span className="data-task-dates">
            {" · "}
            <span className="data-span">{span}</span>
          </span>
        </div>
      </td>
      <td className="data-state">
        <StatusBadge tone={tones[batch.state] ?? "neutral"}>
          {states[batch.state] ? t(states[batch.state]!) : t("未知")}
        </StatusBadge>
        {batch.members.some(item => item.error) && (
          <details>
            <summary>{t("错误详情")}</summary>
            {batch.members
              .filter(item => item.error)
              .map(item => (
                <p key={item.request!.id} className="alert">
                  {tableName(item.request!.table)} ·{" "}
                  <ErrorNotice
                    error={remoteError(item.error!)}
                    namespace="asterion.terminal.data-workbench"
                  />
                </p>
              ))}
          </details>
        )}
      </td>
      <td className="data-metric subtle">
        <span>
          {batch.items > 0n
            ? `${Math.min(100, Math.floor((Number(batch.itemsDone) * 100) / Number(batch.items)))}% · ${t(
                "{done} / {total} 个分区",
                {
                  done: batch.itemsDone.toString(),
                  total: batch.items.toString(),
                },
              )}`
            : batch.requestsSent > 0n
              ? t("已请求 {count} 次", { count: batch.requestsSent.toString() })
              : "—"}
          {dailyEmpty > 0n && ` · ${t("{count} 个分区暂无数据", { count: dailyEmpty.toString() })}`}
          {weeklyNote && ` · ${weeklyNote}`}
        </span>
        {running && (
          <progress
            className="data-progress"
            aria-label={title}
            value={batch.items > 0n ? Number(batch.itemsDone) : undefined}
            max={batch.items > 0n ? Number(batch.items) : undefined}
          />
        )}
      </td>
      <td className="data-row-action">
        {running && (
          <button
            type="button"
            className="ghost-action"
            onClick={() => void cancel(batch.activeIds)}
          >
            {t("取消")}
          </button>
        )}
      </td>
    </tr>
  );
}
