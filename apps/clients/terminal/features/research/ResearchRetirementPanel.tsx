import { useEffect, useRef, useState } from "react";
import {
  BackendError,
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { ResearchRetirement, TaskOwnerRef } from "../../src/bridge/client";
const namespace = "asterion.terminal.research";
const t = (key: string) => translate(namespace, key);
export function ResearchRetirementPanel({
  id,
  owner,
  busy,
  query,
  trade,
}: { id: string; owner: TaskOwnerRef } & Pick<TerminalContext, "busy" | "query" | "trade">) {
  const [opened, setOpened] = useState(false);
  const [report, setReport] = useState<ResearchRetirement | null>(null);
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
  async function load() {
    const current = ++generation.current;
    setReport(null);
    setConfirmed(false);
    setError("");
    setPending(true);
    try {
      const reply = await query("task.retirement.preview", { id, owner });
      if (current !== generation.current) return;
      if (!reply.task_retirement || reply.task_retirement.id !== id)
        throw new Error(t("研究维护预览缺失，请重新核对"));
      setReport(reply.task_retirement);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setPending(false);
    }
  }
  async function execute() {
    if (
      !report ||
      (report.state !== "available" && report.state !== "retired") ||
      (report.state === "available" && !confirmed)
    )
      return;
    const current = ++generation.current;
    setPending(true);
    setConfirmed(false);
    setError("");
    try {
      await trade("task.retirement.execute", { id, owner });
      if (current !== generation.current) return;
      await load();
    } catch (reason) {
      if (current === generation.current) {
        setReport(null);
        setError(asDisplayError(reason));
      }
    } finally {
      if (current === generation.current) setPending(false);
    }
  }
  return (
    <div className="research-retirement">
      <button
        disabled={busy || pending}
        onClick={() => {
          setOpened(true);
          void load();
        }}
      >
        {t("维护研究记录")}
      </button>
      {opened && (
        <section aria-label={t("研究记录维护")}>
          <code style={{ overflowWrap: "anywhere" }}>{id}</code>
          <p>
            {t(
              "退役保留尝试、选择与原输入结果证据，禁止重试和新增派生；不会删除载荷或级联删除 Data 内容。",
            )}
          </p>
          {pending && <p role="status">{t("正在核对研究维护状态…")}</p>}
          {error && (
            <p role="alert">
              <ErrorNotice error={error} namespace={namespace} />
            </p>
          )}
          {report && (
            <>
              <p role="status">
                {t(
                  report.state === "retired"
                    ? "已退役"
                    : report.state === "blocked"
                      ? "存在活动或派生引用，禁止退役"
                      : "可退役",
                )}
              </p>
              <p>
                <code style={{ overflowWrap: "anywhere" }}>
                  {report.node} / {report.service} / {report.version}
                </code>
              </p>
              {report.active && <p>{t("任务尚未进入终态")}</p>}
              {report.admission_busy && <p>{t("接纳或维护正在提交，请稍后重新核对")}</p>}
              {report.consumers.length > 0 && (
                <>
                  <p>{t("仍被以下研究记录引用")}</p>
                  {report.consumers.length === 257 && (
                    <p>{t("引用列表达到显示上限，可能存在更多阻断项")}</p>
                  )}
                  <ul>
                    {report.consumers.map(consumer => (
                      <li key={consumer}>
                        <code>{consumer}</code>
                      </li>
                    ))}
                  </ul>
                </>
              )}
              {report.state === "available" && (
                <>
                  <label>
                    <input
                      type="checkbox"
                      checked={confirmed}
                      disabled={busy || pending}
                      onChange={event => setConfirmed(event.target.checked)}
                    />
                    {t("确认退役此记录，禁止重试和新增派生")}
                  </label>
                  <button disabled={busy || pending || !confirmed} onClick={() => void execute()}>
                    {t("确认退役")}
                  </button>
                </>
              )}
              {report.state === "retired" && (
                <>
                  <p>
                    {t(
                      report.source_release_confirmed
                        ? "Data 已确认解除源保留"
                        : "源保留尚未确认解除，来源内容继续受保护",
                    )}
                  </p>
                  {report.release_error && (
                    <ErrorNotice
                      error={
                        new BackendError(report.release_error.code, report.release_error.message)
                      }
                    />
                  )}
                  {!report.source_release_confirmed && (
                    <button disabled={busy || pending} onClick={() => void execute()}>
                      {t("重试解除源保留")}
                    </button>
                  )}
                </>
              )}
            </>
          )}
          <button
            disabled={pending}
            onClick={() => {
              ++generation.current;
              setOpened(false);
              setReport(null);
              setConfirmed(false);
            }}
          >
            {t("关闭维护")}
          </button>
        </section>
      )}
    </div>
  );
}
