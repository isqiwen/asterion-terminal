import { useEffect, useRef, useState } from "react";
import type { Snapshot, TaskOwnerRef } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function AcquisitionRetirement({
  id,
  owner,
  busy,
  query,
  trade,
}: {
  id: string;
  owner: TaskOwnerRef;
} & Pick<TerminalContext, "busy" | "query" | "trade">) {
  const [report, setReport] = useState<Snapshot["task_retirement"]>();
  const [pending, setPending] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const generation = useRef(0);
  useEffect(
    () => () => {
      ++generation.current;
    },
    [],
  );
  async function inspect(execute = false) {
    const current = ++generation.current;
    setPending(true);
    setConfirmed(false);
    setError("");
    setReport(undefined);
    try {
      if (execute) await trade("task.retirement.execute", { id, owner });
      const reply = await query("task.retirement.preview", { id, owner });
      if (current !== generation.current) return;
      const value = reply.task_retirement;
      if (!value || value.id !== id || value.service !== owner.service)
        throw new Error(t("采集维护预览缺失，请重新核对"));
      setReport(value);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setPending(false);
    }
  }
  return (
    <div>
      <button disabled={busy || pending} onClick={() => void inspect()}>
        {t("维护采集记录")}
      </button>
      {pending && <p role="status">{t("正在核对采集维护状态…")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {report && (
        <section aria-label={t("采集记录维护")}>
          <code>{id}</code>
          <p>{t("退役保留授权、尝试与原输入结果证据，禁止重试；不退款、不删除产出数据。")}</p>
          <p role="status">
            {t(
              report.state === "retired"
                ? "采集记录已退役"
                : report.state === "blocked"
                  ? "采集仍活动或正在提交，禁止退役"
                  : "采集记录可退役",
            )}
          </p>
          {report.state === "available" && (
            <>
              <label>
                <input
                  type="checkbox"
                  checked={confirmed}
                  disabled={busy || pending}
                  onChange={event => setConfirmed(event.target.checked)}
                />
                {t("确认退役采集记录并禁止重试")}
              </label>
              <button
                disabled={busy || pending || !confirmed}
                onClick={() => {
                  if (confirmed) void inspect(true);
                }}
              >
                {t("确认采集退役")}
              </button>
            </>
          )}
        </section>
      )}
    </div>
  );
}
