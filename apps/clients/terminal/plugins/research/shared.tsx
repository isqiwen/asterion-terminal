import { useState, type ReactNode } from "react";
import {
  TaskPagination,
  translate,
  getLocale,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import type { TerminalCommand } from "../../src/bridge/client";

export const namespace = "asterion.terminal.research";
export const t = (key: string, values?: MessageValues) => translate(namespace, key, values);
export const states: Record<string, string> = {
  queued: "排队中",
  running: "运行中",
  cancel_requested: "正在取消",
  publishing: "正在发布",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
/**
 * What a selection is studied as: each contract outside a dominant series is
 * a series (`alone`), and each dominant series is one, whatever months it is
 * read from.
 */
export function selectedSeries(snapshot: TerminalContext["snapshot"]) {
  const dominant = snapshot?.dataset_series ?? [];
  const alone = (snapshot?.datasets ?? []).filter(
    item =>
      !dominant.some(series => series.venue === item.venue && series.symbols.includes(item.symbol)),
  );
  return { alone, dominant, count: alone.length + dominant.length };
}
// A factor counts in observations of its series: the same parameters read as
// bars or as trading days, by what the series is made of.
export const factorWords = {
  bars: {
    computed: "按 K 线计算",
    lookback: "回看 K 线数",
    horizon: "未来收益 K 线数",
    split: "前段 K 线数",
    training: "训练 K 线数",
    validation: "每轮验证 K 线数",
    candidates: "候选回看 K 线数",
    selected: "选中回看 K 线数",
    warmup: "共同预热 K 线数",
    input: "输入 K 线",
    observed: "因子时间",
    label: "标签结束时间",
  },
  daily: {
    computed: "按日线计算",
    lookback: "回看日线数",
    horizon: "未来日线数",
    split: "前段日线数",
    training: "训练日线数",
    validation: "每轮验证日线数",
    candidates: "候选回看日线数",
    selected: "选中回看日线数",
    warmup: "共同预热日线数",
    input: "输入日线",
    observed: "交易日",
    label: "标签日期",
  },
} as const;
type TaskService = NonNullable<NonNullable<TerminalContext["snapshot"]>["task_service"]>;
export type ResearchTask = TaskService["tasks"][number];
export type Run = (method: TerminalCommand, params?: Record<string, unknown>) => Promise<boolean>;

/** Runs commands for one page; a failure becomes the page's error instead of an exception. */
export function useRun(trade: TerminalContext["trade"]) {
  const [error, setError] = useState<DisplayError>("");
  const run: Run = async (method, params = {}) => {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  return { error, setError, run };
}

/** A research page: its title, where its tasks run, and its last error. */
export function ResearchPage({
  title,
  taskService,
  error,
  children,
}: {
  title: string;
  taskService: TaskService | null | undefined;
  error: DisplayError;
  children: ReactNode;
}) {
  return (
    <section className="research-workspace" aria-label={t("期货研究")}>
      <div className="panel-heading">
        <h2>{title}</h2>
        <span className="panel-spacer" />
        {taskService && (
          <span className="subtle">
            {taskService.remote ? `${taskService.host} · ${taskService.service}` : t("本机")}
          </span>
        )}
        <span role="status">{taskService?.online ? t("任务服务已连接") : t("任务服务未连接")}</span>
      </div>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace={namespace} />
        </p>
      )}
      {children}
    </section>
  );
}

/** The tasks a page started, newest first, with the actions their state allows. */
export function TaskRecords({
  heading,
  taskService,
  busy,
  trade,
  run,
  listed,
  describe,
  onProgress,
  onResult,
}: Pick<TerminalContext, "busy" | "trade"> & {
  heading?: string;
  taskService: TaskService | null | undefined;
  run: Run;
  listed: (task: ResearchTask) => boolean;
  describe?: (task: ResearchTask) => string;
  onProgress?: (task: ResearchTask) => void;
  onResult: (task: ResearchTask) => void;
}) {
  return (
    <section aria-label={t("研究任务")}>
      {heading && <h3>{heading}</h3>}
      <TaskPagination taskService={taskService} busy={busy} trade={trade} />
      {!taskService?.tasks.some(listed) ? (
        <p className="research-empty">{t("暂无研究任务")}</p>
      ) : (
        <div className="research-table">
          <table>
            <thead>
              <tr>
                <th>{t("合约 / 交易日")}</th>
                <th>{t("提交时间")}</th>
                <th>{t("状态")}</th>
                <th>{t("进度")}</th>
                <th>{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {[...taskService.tasks]
                .filter(listed)
                .reverse()
                .map(task => (
                  <tr key={task.id}>
                    <td>
                      <strong>{task.instrument}</strong>
                      {describe && <span className="subtle">{describe(task)}</span>}
                      <details>
                        <summary>{t("详情")}</summary>
                        <code>{task.id}</code>
                        <p>{t("执行次数：{count}", { count: task.attempt })}</p>
                        <p>
                          {t("最近更新")} ·{" "}
                          {new Date(task.updated_at_ms).toLocaleString(getLocale())}
                        </p>
                        {task.error && <p>{task.error}</p>}
                      </details>
                    </td>
                    <td>
                      <time dateTime={new Date(task.submitted_at_ms).toISOString()}>
                        {new Date(task.submitted_at_ms).toLocaleString(getLocale())}
                      </time>
                    </td>
                    <td>
                      {t(states[task.state] ?? task.state)}
                      {!taskService.online && <span className="subtle">{t("最后确认状态")}</span>}
                    </td>
                    <td>
                      {task.completed} / {task.total}
                    </td>
                    <td>
                      {onProgress && task.state !== "succeeded" && (
                        <button disabled={busy} onClick={() => onProgress(task)}>
                          {t("查看进度")}
                        </button>
                      )}
                      {task.state === "succeeded" && (
                        <button
                          disabled={busy || !taskService.online}
                          onClick={() => onResult(task)}
                        >
                          {t("查看结果")}
                        </button>
                      )}
                      {["queued", "running"].includes(task.state) && (
                        <button
                          disabled={busy || !taskService.online}
                          onClick={() => void run("task.action", { id: task.id, action: "cancel" })}
                        >
                          {t("取消")}
                        </button>
                      )}
                      {["failed", "cancelled", "interrupted"].includes(task.state) && (
                        <button
                          disabled={busy || !taskService.online}
                          onClick={() => void run("task.action", { id: task.id, action: "retry" })}
                        >
                          {t("重新运行")}
                        </button>
                      )}
                    </td>
                  </tr>
                ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
