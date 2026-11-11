import { TaskState } from "../../src/generated/asterion/warehouse_pb";
import { warehouseData } from "../../src/bridge/warehouse-data";
import type { AcquisitionTask } from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { type MessageValues, translate } from "../../src/i18n";

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
export const activeTask = (task: AcquisitionTask) =>
  [TaskState.QUEUED, TaskState.RUNNING].includes(task.state);

// Every client's tasks, newest first.
export function Tasks({
  tasks,
  tableName,
  canManage,
  older,
  refresh,
}: {
  tasks: AcquisitionTask[];
  tableName: (table: number) => string;
  canManage: boolean;
  older?: () => void;
  refresh: () => Promise<void>;
}) {
  const cancelled = useActionError();
  const cancel = (id: string) =>
    cancelled.attempt(async () => {
      await warehouseData({ case: "cancelAcquisition", value: { id } });
      await refresh();
    });
  return (
    <section className="data-tasks" aria-label={t("采集任务")}>
      <ActionError error={cancelled.error} />
      {tasks.length ? (
        <table className="data-table" aria-label={t("采集任务")}>
          <thead>
            <tr>
              <th>{t("表")}</th>
              <th>{t("范围")}</th>
              <th>{t("区间")}</th>
              <th>{t("状态")}</th>
              <th>{t("进度")}</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {tasks.map(task => {
              const request = task.request!;
              return (
                <tr
                  key={request.id}
                  title={t("任务 {id} · 请求 {count} 次", {
                    id: request.id,
                    count: task.requestsSent.toString(),
                  })}
                >
                  <td>{tableName(request.table)}</td>
                  <td>
                    {[...request.exchanges, ...request.instruments].join("、") || t("全部交易所")}
                  </td>
                  <td>
                    {request.firstDay || t("上市日")} – {request.lastDay || t("摘牌日")}
                  </td>
                  <td data-state={TaskState[task.state].toLowerCase()}>
                    {t(states[task.state] ?? String(task.state))}
                    {task.error && <div className="task-error">{task.error.message}</div>}
                  </td>
                  <td>
                    {task.itemsDone.toString()} / {task.items.toString()}
                    {task.itemsEmpty > 0n &&
                      ` · ${t("{count} 天尚无数据", { count: Number(task.itemsEmpty) })}`}
                  </td>
                  <td>
                    {canManage && activeTask(task) && (
                      <button type="button" onClick={() => void cancel(request.id)}>
                        {t("取消")}
                      </button>
                    )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      ) : (
        <p className="subtle">{t("还没有采集任务。")}</p>
      )}
      {older && (
        <button type="button" onClick={older}>
          {t("更早的任务")}
        </button>
      )}
    </section>
  );
}
