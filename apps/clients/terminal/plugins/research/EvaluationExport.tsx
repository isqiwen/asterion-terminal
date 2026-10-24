import { useEffect, useRef, useState } from "react";
import type { TaskResult, TaskOwnerRef, ResearchProjectConclusion } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  nativeDesktop,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { saveResearchExport } from "./ResearchExport";
import { namespace, t } from "./shared";

type Report = Extract<TaskResult, { kind: "performance_evaluation" }>;
export function EvaluationExport({
  report,
  owner,
  conclusion,
  query,
  busy,
}: {
  report: Report;
  owner: TaskOwnerRef;
  conclusion?: ResearchProjectConclusion;
} & Pick<TerminalContext, "query" | "busy">) {
  const [format, setFormat] = useState<"html" | "csv">("html");
  const [confirmed, setConfirmed] = useState(false);
  const [pending, setPending] = useState(false);
  const [receipt, setReceipt] = useState<{ bytes: number; target: string } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const identity = JSON.stringify([
    owner,
    report.id,
    report.task.result_digest,
    conclusion?.digest,
    format,
  ]);
  useEffect(() => {
    ++epoch.current.value;
    setConfirmed(false);
    setReceipt(null);
    setError("");
    setPending(false);
    const token = epoch.current;
    return () => {
      ++token.value;
    };
  }, [identity]);
  async function exportFile() {
    if (!confirmed || pending) return;
    const current = epoch.current.value;
    setPending(true);
    setReceipt(null);
    setError("");
    let target = `${report.id}.evaluation.${format}`;
    try {
      const result = await saveResearchExport(
        async path => {
          const reply = await query("evaluation.report.export", {
            id: report.id,
            owner,
            version: report.task.result_digest,
            format,
            conclusion_id: conclusion?.declaration.id ?? "",
            conclusion_version: conclusion?.digest ?? "",
            path,
          });
          if (
            reply.research_export?.source_version !== report.task.result_digest ||
            reply.research_export.conclusion_version !== (conclusion?.digest ?? "")
          )
            throw new Error(t("评价报告导出确认不匹配"));
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
    <section aria-label={t("固定评价本地导出")}>
      <h4>{t("固定评价本地导出")}</h4>
      <p>
        {t(
          "导出当前固定绩效及原限制，不是完整统一评价报告；不含账户标识、配置、原行情或完整图表，不自动发送或授予交易资格。",
        )}
      </p>
      <p>
        <code>
          {report.id} / {report.task.result_digest}
        </code>
      </p>
      {conclusion ? (
        <p>
          {t("附加所选固定人工解释，原计算限制不改写")} ·{" "}
          <code>
            {conclusion.declaration.id} / {conclusion.digest}
          </code>{" "}
          · {conclusion.declaration.reason}
        </p>
      ) : (
        <p>{t("此次不附人工判断；报告保留未人工审阅状态。")}</p>
      )}
      {report.result.ledger && (
        <p>
          {t("账本导出包含全部原日结、成交及委托身份、金额和未解释差额；页面分页不缩小文件范围。")}
        </p>
      )}
      {report.result.contributions && (
        <p>
          {t(
            "合约贡献导出包含全部原合约、交易日、成交、核对项及未解释差额；页面筛选和分页不缩小固定账户范围。",
          )}
        </p>
      )}
      <label>
        {t("固定评价导出格式")}
        <select
          disabled={pending}
          value={format}
          onChange={event => setFormat(event.target.value as "html" | "csv")}
        >
          <option value="html">{t("离线HTML（固定字段与方法）")}</option>
          <option value="csv">{t("CSV（固定指标明细与状态）")}</option>
        </select>
      </label>
      <p>
        {t(
          "HTML和CSV均保留缺项、失败原因及可选人工解释。请选择新本地文件，已有文件不会覆盖；失败不改变已保存报告。",
        )}
      </p>
      <label>
        <input
          type="checkbox"
          checked={confirmed}
          disabled={pending}
          onChange={event => setConfirmed(event.target.checked)}
        />
        {t("已核对固定报告、人工解释范围及全部限制，明确导出本地文件")}
      </label>
      <button disabled={busy || pending || !confirmed} onClick={() => void exportFile()}>
        {t("导出固定评价报告")}
      </button>
      {pending && (
        <p role="status">
          {t("正在导出固定报告，请勿重复提交")} ·{" "}
          <code>
            {report.id} / {report.task.result_digest}
          </code>
        </p>
      )}
      <ErrorNotice error={error} namespace={namespace} />
      {receipt && (
        <p role="status">
          {t(nativeDesktop ? "固定评价报告已保存" : "固定评价报告下载已发起")} · {receipt.bytes}{" "}
          bytes · {receipt.target}
        </p>
      )}
    </section>
  );
}
