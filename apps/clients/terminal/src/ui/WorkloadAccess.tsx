import { useEffect, useRef, useState, type ReactNode } from "react";
import { translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { TerminalContext } from "../../plugins/contract";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// Entering a workflow prepares its selected owner once. A remote
// connection is never replaced and failed attempts require an explicit retry.
export function WorkloadAccess({
  context,
  enabled = true,
  role = "research",
  children,
}: {
  context: TerminalContext;
  enabled?: boolean;
  role?: "data" | "research";
  children: (context: TerminalContext) => ReactNode;
}) {
  const { snapshot, busy, trade } = context;
  const taskService = role === "data" ? snapshot?.data : snapshot?.research_service;
  const online = !!taskService?.online;
  const remote = !!taskService?.remote;
  const attempted = useRef<string | null>(null);
  const [preparing, setPreparing] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  // Reopening creates a new connection ID while retaining the selected owner.
  const identity = JSON.stringify([
    taskService?.remote,
    taskService?.host,
    taskService?.port,
    taskService?.service,
  ]);
  function prepare() {
    attempted.current = identity;
    setPreparing(true);
    setError("");
    void trade(role === "data" ? "node.data.local.open" : "node.research.local.open")
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
      {enabled &&
        taskService?.online &&
        capacity &&
        (remaining * 10 < capacity.active_limit || capacity.uncommitted > 0) && (
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
                ? "远程数据或研究连接已断开，当前运行位置保持不变。"
                : preparing || attempted.current === null
                  ? "正在准备数据与研究服务…"
                  : "数据与研究服务未就绪。",
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
