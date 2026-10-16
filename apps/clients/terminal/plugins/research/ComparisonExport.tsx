import { useState, type RefObject } from "react";
import type { TaskResult } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  nativeDesktop,
  getLocale,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { saveResearchExport } from "./ResearchExport";
import { namespace, t } from "./shared";
type Evidence = Extract<TaskResult, { kind: "factor" | "backtest" }>;
export function captureComparison(
  table: HTMLTableElement,
  evidence: Evidence[],
  reasons: string[][],
) {
  return {
    language: getLocale(),
    reference_id: evidence[0].id,
    columns: evidence.map((v, i) => ({
      id: v.id,
      methodology: reasons[i].length ? "independent" : "comparable",
      reasons: reasons[i].map(reason => t(reason)),
    })),
    rows: Array.from(table.tBodies[0].rows).map(row => ({
      label: row.cells[0].textContent?.trim() ?? "",
      cells: Array.from(row.cells)
        .slice(1)
        .map(cell => ({
          text: cell.textContent?.trim() ?? "",
          state: cell.dataset.valueState ?? "display_only",
          metrics: Array.from(cell.querySelectorAll<HTMLElement>("dd[data-value-state]")).map(
            value => ({
              label: value.previousElementSibling?.textContent?.trim() ?? "",
              text: value.textContent?.trim() ?? "",
              state: value.dataset.valueState!,
            }),
          ),
        })),
    })),
  };
}
export function ComparisonExport({
  table,
  evidence,
  reasons,
  busy,
  query,
}: { table: RefObject<HTMLTableElement | null>; evidence: Evidence[]; reasons: string[][] } & Pick<
  TerminalContext,
  "busy" | "query"
>) {
  const [pending, setPending] = useState(false),
    [receipt, setReceipt] = useState<{ sha256: string; bytes: number } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  async function exportFile() {
    if (!table.current) return;
    setPending(true);
    setReceipt(null);
    setError("");
    try {
      const matrix = captureComparison(table.current, evidence, reasons);
      const expected = evidence.map(v => ({
        id: v.id,
        result_digest: v.task.result_digest,
        metadata_revision: v.task.metadata?.revision ?? 0,
        input_sha256: v.kind === "backtest" ? v.input_sha256 : null,
      }));
      setReceipt(
        await saveResearchExport(
          path => query("task.comparison.export", { expected, matrix, path }),
          "research-comparison.evidence.json",
        ),
      );
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <section aria-label={t("比较证据导出")}>
      <p>
        {t(
          "导出当前矩阵、逐指标状态、口径原因及每列核验后的实验证据摘要。不是独立复现包；名称备注原样包含。任一实验读取失败或描述已变化时，请重新读取比较。已有文件不会覆盖。",
        )}
      </p>
      <button disabled={busy || pending || evidence.length < 2} onClick={() => void exportFile()}>
        {t("导出当前比较证据")}
      </button>
      {receipt && (
        <p role="status">
          {t(nativeDesktop ? "研究证据已保存" : "研究证据下载已发起")} · {receipt.bytes} bytes ·
          SHA-256 <code>{receipt.sha256}</code>
        </p>
      )}
      <ErrorNotice error={error} namespace={namespace} />
    </section>
  );
}
