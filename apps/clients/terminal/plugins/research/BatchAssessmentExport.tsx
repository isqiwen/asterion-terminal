import { useEffect, useRef, useState } from "react";
import type { ResearchBatchAssessment } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  nativeDesktop,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { saveResearchExport } from "./ResearchExport";
import { namespace, t } from "./shared";

export function BatchAssessmentExport({
  assessment,
  query,
  busy,
}: { assessment: ResearchBatchAssessment } & Pick<TerminalContext, "query" | "busy">) {
  const [format, setFormat] = useState<"html" | "csv">("html");
  const [confirmed, setConfirmed] = useState(false);
  const [pending, setPending] = useState(false);
  const [receipt, setReceipt] = useState<{ bytes: number; target: string } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const identity = JSON.stringify([assessment.object, format]);
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setConfirmed(false);
    setPending(false);
    setReceipt(null);
    setError("");
    return () => {
      ++token.value;
    };
  }, [identity]);
  async function exportFile() {
    if (busy || pending || !confirmed) return;
    const current = epoch.current.value;
    setPending(true);
    setReceipt(null);
    setError("");
    let target = `${assessment.object.id}.assessment.${format}`;
    try {
      const result = await saveResearchExport(
        async path => {
          const reply = await query("research.batch.assessment.export", {
            object: assessment.object,
            format,
            path,
          });
          if (
            reply.research_export?.source_version !== assessment.object.version ||
            reply.research_export.conclusion_version !== ""
          )
            throw new Error(t("固定批次审阅导出确认不匹配"));
          target = reply.research_export.path ?? reply.research_export.name;
          return reply;
        },
        target,
        format,
      );
      if (current === epoch.current.value && result) setReceipt({ ...result, target });
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  return (
    <section aria-label={t("固定批次审阅本地导出")}>
      <h5>{t("固定批次审阅本地导出")}</h5>
      <p>
        <code>
          {assessment.object.id} / {assessment.object.version}
        </code>
      </p>
      <p>
        {t(
          "导出全部声明、成功或缺失状态、原条件及人工判断，不重算、不排名。文件原样保留人工文字、原诊断及稳定身份，请核对敏感范围；不附连接配置、行情原件或程序，不自动发送或授权交易。",
        )}
      </p>
      <label>
        {t("批次审阅导出格式")}
        <select
          disabled={pending}
          value={format}
          onChange={event => setFormat(event.target.value as "html" | "csv")}
        >
          <option value="html">{t("离线HTML（固定字段与方法）")}</option>
          <option value="csv">{t("CSV（完整声明及状态）")}</option>
        </select>
      </label>
      <p>{t("请选择新本地文件，已有文件不会覆盖；导出失败不改变固定审阅或成员。")}</p>
      <label>
        <input
          type="checkbox"
          checked={confirmed}
          disabled={pending}
          onChange={event => setConfirmed(event.target.checked)}
        />
        {t("已核对固定审阅、原条件、人工文字及诊断范围，明确导出本地文件")}
      </label>
      <button disabled={busy || pending || !confirmed} onClick={() => void exportFile()}>
        {t("导出固定批次审阅")}
      </button>
      {pending && <p role="status">{t("正在导出固定报告，请勿重复提交")}</p>}
      <ErrorNotice error={error} namespace={namespace} />
      {receipt && (
        <p role="status">
          {t(nativeDesktop ? "固定批次审阅已保存" : "固定批次审阅下载已发起")} · {receipt.bytes}{" "}
          bytes · {receipt.target}
        </p>
      )}
    </section>
  );
}
