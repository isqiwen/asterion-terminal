import { useCallback, useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
  type MessageValues,
} from "../contract";
import type { MinuteRetirement, MinuteRetirements } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states = {
  available: "无阻断项",
  blocked: "有关联记录，禁止删除",
  pending: "已封存，清理尚未完成",
  removed: "载荷已删除，来源证据保留",
};

export function MinuteRetirementPanel({
  context,
  id,
  onClose,
  onRemoved,
}: {
  context: TerminalContext;
  id: string;
  onClose: () => void;
  onRemoved: () => void;
}) {
  const [report, setReport] = useState<MinuteRetirement | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [busy, setBusy] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const generation = useRef({ value: 0 });
  const currentContext = useRef(context);
  currentContext.current = context;
  const load = useCallback(async () => {
    const current = ++generation.current.value;
    setReport(null);
    setConfirmed(false);
    setError("");
    setBusy(true);
    try {
      const reply = await currentContext.current.query("data.history.retirement.preview", { id });
      if (current !== generation.current.value) return;
      if (!reply.minute_retirement || reply.minute_retirement.id !== id)
        throw new Error(t("维护预览缺失，请重新核对"));
      setReport(reply.minute_retirement);
    } catch (e) {
      if (current === generation.current.value) setError(asDisplayError(e));
    } finally {
      if (current === generation.current.value) setBusy(false);
    }
  }, [id]);
  useEffect(() => {
    const counter = generation.current;
    void load();
    return () => {
      ++counter.value;
    };
  }, [load]);
  const execute = async () => {
    if (
      !confirmed ||
      !report ||
      report.selected ||
      (report.state !== "available" && report.state !== "pending")
    )
      return;
    const current = ++generation.current.value;
    setBusy(true);
    setConfirmed(false);
    setError("");
    try {
      await currentContext.current.trade("data.history.retirement.execute", { id });
      if (current !== generation.current.value) return;
      const reply = await currentContext.current.query("data.history.retirement.preview", { id });
      if (current !== generation.current.value) return;
      if (!reply.minute_retirement || reply.minute_retirement.id !== id)
        throw new Error(t("维护预览缺失，请重新核对"));
      setReport(reply.minute_retirement);
      if (reply.minute_retirement.state === "removed") onRemoved();
    } catch (e) {
      if (current === generation.current.value) {
        setReport(null);
        setError(asDisplayError(e));
      }
    } finally {
      if (current === generation.current.value) setBusy(false);
    }
  };
  const allowed =
    !!report && !report.selected && (report.state === "available" || report.state === "pending");
  return (
    <section className="history-usage" aria-label={t("分钟版本维护")} aria-busy={busy}>
      <div className="source-actions">
        <h3>{t("分钟版本维护")}</h3>
        <button type="button" onClick={onClose}>
          {t("关闭")}
        </button>
      </div>
      <p>
        {context.snapshot?.data?.node_id} / {context.snapshot?.data?.service}
      </p>
      <code>{id}</code>
      <p>{t("仅清理此分钟版本的载荷；来源清单和封存决定保留，不级联删除关联记录。")}</p>
      <p>{t("执行时重新核对引用和活动读取；预览不能绕过保护。")}</p>
      {busy && <p role="status">{t("正在核对或清理…")}</p>}
      {error && (
        <div role="alert">
          <ErrorNotice error={error} />
          <p>{t("尚未确认清理完成，请刷新原服务的状态；不要直接重复提交。")}</p>
        </div>
      )}
      {report && (
        <>
          <p role="status">{t(states[report.state])}</p>
          {report.payload_bytes !== null && (
            <p>
              {t("当前载荷占用：{bytes} 字节；不含保留的来源证据。", {
                bytes: report.payload_bytes,
              })}
            </p>
          )}
          {report.selected && <p role="alert">{t("本窗口草稿正在使用此版本，请先清除选择。")}</p>}
          <ul>
            {report.references.map(reference => (
              <li key={`${reference.kind}:${reference.id}`}>
                {reference.name} · {reference.id}
              </li>
            ))}
          </ul>
          <ul>
            {report.consumers.map(consumer => (
              <li key={`${consumer.node}:${consumer.service}:${consumer.id}:${consumer.version}`}>
                {consumer.node} / {consumer.service} · {consumer.kind} / {consumer.id} ·{" "}
                {t(consumer.phase === "prepared" ? "准备中的引用仍保留" : "正式引用仍保留")}
              </li>
            ))}
          </ul>
        </>
      )}
      {allowed && (
        <label>
          <input
            type="checkbox"
            checked={confirmed}
            disabled={busy || context.busy}
            onChange={event => setConfirmed(event.target.checked)}
          />
          {t("我确认只清理上方列出的分钟版本")}
        </label>
      )}
      <div className="source-actions">
        <button type="button" disabled={busy || context.busy} onClick={() => void load()}>
          {t("刷新维护预览")}
        </button>
        {allowed && (
          <button
            type="button"
            disabled={!confirmed || busy || context.busy}
            onClick={() => void execute()}
          >
            {t(report?.state === "pending" ? "继续清理" : "删除此版本")}
          </button>
        )}
      </div>
    </section>
  );
}

export function MinuteRetirementList({
  context,
  onOpen,
}: {
  context: TerminalContext;
  onOpen: (id: string) => void;
}) {
  const [page, setPage] = useState<MinuteRetirements | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const generation = useRef({ value: 0 });
  useEffect(() => {
    const counter = generation.current;
    return () => {
      ++counter.value;
    };
  }, []);
  const load = async (after: string) => {
    const current = ++generation.current.value;
    setBusy(true);
    setError("");
    try {
      const reply = await context.query("data.history.retirements", { after });
      if (current !== generation.current.value) return;
      if (!reply.minute_retirements) throw new Error(t("维护预览缺失，请重新核对"));
      const value = reply.minute_retirements;
      setPage(previous => ({
        ...value,
        items: after ? [...(previous?.items ?? []), ...value.items] : value.items,
      }));
    } catch (e) {
      if (current === generation.current.value) setError(asDisplayError(e));
    } finally {
      if (current === generation.current.value) setBusy(false);
    }
  };
  return (
    <section aria-label={t("封存与清理记录")}>
      <button
        type="button"
        disabled={busy || context.busy || !context.snapshot?.data?.online}
        onClick={() => void load("")}
      >
        {t("查看封存与清理记录")}
      </button>
      {error && <ErrorNotice error={error} />}
      {page && (
        <>
          <p>{t("待完成清理可在重启后继续；已删除版本仅保留来源证据。")}</p>
          {!page.items.length && <p>{t("没有封存记录")}</p>}
          <ul>
            {page.items.map(item => (
              <li key={item.id}>
                <code>{item.id}</code> · {t(states[item.state])}{" "}
                <button type="button" onClick={() => onOpen(item.id)}>
                  {t("核对维护状态")}
                </button>
              </li>
            ))}
          </ul>
          {!!page.next_after && (
            <button type="button" disabled={busy} onClick={() => void load(page.next_after)}>
              {t("更多封存记录")}
            </button>
          )}
        </>
      )}
    </section>
  );
}
