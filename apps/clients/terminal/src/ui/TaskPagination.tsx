import { useState } from "react";
import type { Snapshot, TerminalCommand } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
export function TaskPagination({
  taskService,
  busy,
  trade,
}: {
  taskService: Snapshot["task_service"] | undefined;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const [error, setError] = useState<DisplayError>("");
  if (!taskService || (!taskService.before_sequence && !taskService.next_before_sequence))
    return null;
  async function page(before_sequence: number) {
    setError("");
    try {
      await trade("task.page", { before_sequence });
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  return (
    <div className="source-actions">
      <span className="subtle">{t("显示活动任务及本页历史记录")}</span>
      <button
        disabled={busy || !taskService.online || !taskService.before_sequence}
        onClick={() => void page(0)}
      >
        {t("最新记录")}
      </button>
      <button
        disabled={busy || !taskService.online || !taskService.next_before_sequence}
        onClick={() => void page(taskService.next_before_sequence)}
      >
        {t("更早记录")}
      </button>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </div>
  );
}
