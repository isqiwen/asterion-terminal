import type { Snapshot } from "../../src/bridge/client";
import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  nativeDesktop,
  save,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { namespace, t } from "./shared";
export async function saveResearchExport(
  query: (path: string) => Promise<Snapshot>,
  name: string,
  format: "json" | "html" | "csv" = "json",
) {
  let path = "";
  if (nativeDesktop) {
    const chosen = await save({
      title: t("导出研究证据"),
      defaultPath: name,
      filters: [{ name: format.toUpperCase(), extensions: [format] }],
    });
    if (!chosen) return null;
    path = chosen;
  }
  const reply = await query(path);
  const value = reply.research_export;
  if (!value) throw new Error(t("研究证据导出未返回确认"));
  if (!nativeDesktop) {
    if (value.content === undefined) throw new Error(t("研究证据导出未返回文件内容"));
    const url = URL.createObjectURL(
      new Blob([value.content], {
        type: `${format === "html" ? "text/html" : format === "csv" ? "text/csv" : "application/json"};charset=utf-8`,
      }),
    );
    const link = document.createElement("a");
    link.href = url;
    link.download = value.name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  return { bytes: value.bytes };
}
export function ResearchExport({
  id,
  kind,
  busy,
  query,
}: { id: string; kind: "factor" | "backtest" } & Pick<TerminalContext, "busy" | "query">) {
  const [pending, setPending] = useState(false),
    [receipt, setReceipt] = useState<{ bytes: number } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  async function exportFile() {
    setPending(true);
    setReceipt(null);
    setError("");
    try {
      setReceipt(
        await saveResearchExport(
          path => query("task.evidence.export", { id, path }),
          `${id}.evidence.json`,
        ),
      );
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <section aria-label={t("研究证据导出")} className="inline-actions">
      <button disabled={busy || pending} onClick={() => void exportFile()}>
        {t("导出研究证据")}
      </button>
      <details className="subtle">
        <summary>{t("说明")}</summary>
        <p>
          {t(
            "导出版本化 JSON，包含当前名称备注、派生来源、固定参数与快照摘要及完整计算结果。名称备注原样包含，请勿在其中保存凭据。文件不含 input.pb、原始行情、来源表或插件，不能单独复现计算；原格式不支持或证据缺失时拒绝导出。已有目标文件不会覆盖。",
          )}
        </p>
        <p>
          {t(
            kind === "factor"
              ? "因子原始输入摘要未由结果接口提供，导出中明确为 null；结果摘要对应服务保存的 Protobuf，不是导出 JSON。"
              : "回测输入与结果摘要对应服务保存的 Protobuf，不是导出 JSON；全程、留出及滚动样本外口径分别保留。",
          )}
        </p>
      </details>
      {receipt && (
        <p role="status">
          {t(nativeDesktop ? "研究证据已保存" : "研究证据下载已发起")} · {receipt.bytes} bytes
        </p>
      )}
      <ErrorNotice error={error} namespace={namespace} />
    </section>
  );
}
