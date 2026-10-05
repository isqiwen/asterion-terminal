import { useEffect, useRef, useState, type ReactNode } from "react";
import { translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { TerminalContext } from "../../plugins/contract";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// Entering a local data/task workflow prepares its selected pair once. A remote
// connection is never replaced and failed attempts require an explicit retry.
export function DataTaskAccess({
  context,
  enabled = true,
  children,
}: {
  context: TerminalContext;
  enabled?: boolean;
  children: (context: TerminalContext) => ReactNode;
}) {
  const { snapshot, busy, trade } = context;
  const taskService = snapshot?.task_service;
  const dataService = snapshot?.data;
  const online = !!taskService?.online && !!dataService?.online;
  const remote = !!taskService?.remote || !!dataService?.remote;
  const attempted = useRef<string | null>(null);
  const [preparing, setPreparing] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  // Reopening creates new client connection IDs, but still selects the same service pair.
  const identity = JSON.stringify([
    dataService?.remote,
    dataService?.host,
    dataService?.port,
    dataService?.service,
    taskService?.remote,
    taskService?.host,
    taskService?.port,
    taskService?.service,
  ]);
  function prepare() {
    attempted.current = identity;
    setPreparing(true);
    setError("");
    void trade("node.data_tasks.local.open")
      .catch(reason => setError(asDisplayError(reason)))
      .finally(() => setPreparing(false));
  }
  useEffect(() => {
    if (enabled && online) attempted.current = identity;
    if (enabled && snapshot && !busy && !online && !remote && attempted.current !== identity)
      prepare();
    // Admission is keyed by the selected service, not by render callback identity.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, snapshot, busy, online, remote, identity]);
  const unavailable = enabled && !online;
  const capacity = taskService?.capacity;
  const remaining = capacity
    ? capacity.active_limit - capacity.active_used - capacity.active_reserved
    : 0;
  return (
    <>
      {enabled && taskService?.online && capacity && (
        <div className="data-task-access" aria-label={t("活动任务容量")}>
          <span>
            {t("活动名额剩余 {remaining} / {limit}", { remaining, limit: capacity.active_limit })}
          </span>
          {capacity.uncommitted > 0 && (
            <span>
              {t("保留了 {count} 个未完成提交目录，不占用活动名额", {
                count: capacity.uncommitted,
              })}
            </span>
          )}
          <span>{t("已保管 {count} 项任务", { count: capacity.retained_tasks })}</span>
          {remaining === 0 && (
            <span role="status">
              {t("活动任务已满；等待任务结束或取消排队任务后，可在当前服务继续提交。")}
            </span>
          )}
        </div>
      )}
      {unavailable && (
        <div className="data-task-access" role="status">
          <span>
            {t(
              remote
                ? "远程数据或任务连接已断开，当前运行位置保持不变。"
                : preparing || attempted.current === null
                  ? "正在准备数据与任务服务…"
                  : "数据与任务服务未就绪。",
            )}
          </span>
          {error && <ErrorNotice error={error} />}
          {remote ? (
            <button onClick={() => context.openSettings("connections")}>{t("查看连接")}</button>
          ) : (
            !preparing &&
            attempted.current === identity && (
              <button disabled={busy || preparing} onClick={prepare}>
                {t("重试准备")}
              </button>
            )
          )}
        </div>
      )}
      {children({ ...context, busy: busy || preparing })}
    </>
  );
}
