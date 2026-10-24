import { useEffect, useRef, useState } from "react";
import type { TableRetirement, TableRetirementEntry } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function TableSnapshotMaintenance({ context }: { context: TerminalContext }) {
  const [id, setId] = useState("");
  const [report, setReport] = useState<TableRetirement | null>(null);
  const [inventory, setInventory] = useState<TableRetirementEntry[]>([]);
  const [after, setAfter] = useState("");
  const [pending, setPending] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const generation = useRef({ value: 0 });
  const connection = context.snapshot?.data?.connection_id;
  useEffect(() => {
    ++generation.current.value;
    setReport(null);
    setInventory([]);
    setAfter("");
    setConfirmed(false);
    setPending(false);
    setError("");
    const epoch = generation.current;
    return () => {
      ++epoch.value;
    };
  }, [connection]);
  function choose(value: string) {
    ++generation.current.value;
    setId(value);
    setReport(null);
    setConfirmed(false);
    setError("");
  }
  async function inspect(execute = false) {
    const current = ++generation.current.value;
    setPending(true);
    setError("");
    setConfirmed(false);
    setReport(null);
    try {
      if (execute) {
        if (!report?.preview_digest) throw new Error(t("表快照维护预览缺失，请重新核对"));
        await context.trade("data.tables.retirement.execute", {
          id,
          preview_digest: report.preview_digest,
        });
      }
      const reply = await context.query("data.tables.retirement.preview", { id });
      if (current !== generation.current.value) return;
      if (!reply?.table_retirement || reply.table_retirement.id !== id)
        throw new Error(t("表快照维护预览缺失，请重新核对"));
      setReport(reply.table_retirement);
    } catch (reason) {
      if (current === generation.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current.value) setPending(false);
    }
  }
  async function list(cursor = "") {
    const current = ++generation.current.value;
    setPending(true);
    setConfirmed(false);
    setReport(null);
    setError("");
    try {
      const reply = await context.query("data.tables.retirements", { after: cursor });
      if (current === generation.current.value) {
        setInventory(previous =>
          cursor
            ? [...previous, ...(reply.table_retirements?.items ?? [])]
            : (reply.table_retirements?.items ?? []),
        );
        setAfter(reply.table_retirements?.next_after ?? "");
      }
    } catch (reason) {
      if (current === generation.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current.value) setPending(false);
    }
  }
  const busy = pending || context.busy || !context.snapshot?.data?.online;
  const allowed =
    report && !report.selected && (report.state === "available" || report.state === "pending");
  return (
    <details>
      <summary>{t("表快照维护")}</summary>
      <section aria-label={t("表快照维护")}>
        <p>
          {t(
            "快照是账本前缀。只清理不再被任何可用前缀引用的对象，保留修订链和来源证据；共享内容可能释放零字节。",
          )}
        </p>
        <label>
          {t("待维护快照 SHA-256")}
          <input
            value={id}
            disabled={pending}
            onChange={event => choose(event.target.value.trim())}
          />
        </label>
        <button disabled={busy || !/^[0-9a-f]{64}$/.test(id)} onClick={() => void inspect()}>
          {t("预览表快照维护")}
        </button>
        <button disabled={busy} onClick={() => void list()}>
          {t("查看表快照封存记录")}
        </button>
        {pending && <p role="status">{t("正在核对表快照维护…")}</p>}
        {error && (
          <p role="alert">
            <ErrorNotice error={error} />
          </p>
        )}
        {report && (
          <div>
            <code>{report.id}</code>
            <p role="status">
              {t(
                report.state === "removed"
                  ? "快照已退役，独占载荷已清理"
                  : report.state === "pending"
                    ? "快照已封存，清理尚未完成"
                    : report.state === "blocked" || report.selected
                      ? "快照仍被使用，禁止退役"
                      : "无阻断项，可退役快照",
              )}
            </p>
            {report.current_head && <p>{t("当前头快照不能退役")}</p>}
            {report.selected && <p>{t("本窗口草稿正在使用此快照，请先清除选择。")}</p>}
            <p>
              {t("预计独占载荷字节")}: {report.payload_bytes ?? t("未知")}
            </p>
            <ul>
              {report.acquisitions.map(task => (
                <li key={task}>
                  {t("采集记录")}: <code>{task}</code>
                </li>
              ))}
              {report.references.map(ref => (
                <li key={ref.id}>{ref.name || ref.id}</li>
              ))}
              {report.consumers.map(ref => (
                <li key={`${ref.node}/${ref.service}/${ref.id}`}>
                  {ref.domain} / {ref.service} / <code>{ref.id}</code>
                </li>
              ))}
            </ul>
            {report.objects.length > 0 && (
              <details>
                <summary>{t("本次独占对象")}</summary>
                <ul>
                  {report.objects.map(object => (
                    <li key={object}>
                      <code>{object}</code>
                    </li>
                  ))}
                </ul>
              </details>
            )}
            {allowed && (
              <>
                <label>
                  <input
                    type="checkbox"
                    checked={confirmed}
                    disabled={busy}
                    onChange={event => setConfirmed(event.target.checked)}
                  />
                  {t("确认仅退役上方快照及列出的独占对象")}
                </label>
                <button
                  disabled={busy || !confirmed}
                  onClick={() => {
                    if (confirmed) void inspect(true);
                  }}
                >
                  {t(report.state === "pending" ? "继续表快照清理" : "退役此表快照")}
                </button>
              </>
            )}
          </div>
        )}
        {after && (
          <button disabled={busy} onClick={() => void list(after)}>
            {t("继续读取封存记录")}
          </button>
        )}
        {inventory.length > 0 && (
          <ul aria-label={t("表快照封存记录")}>
            {inventory.map(entry => (
              <li key={entry.id}>
                <code>{entry.id}</code> · {entry.state}
                <button disabled={busy} onClick={() => choose(entry.id)}>
                  {t("核对此封存快照")}
                </button>
              </li>
            ))}
          </ul>
        )}
      </section>
    </details>
  );
}
