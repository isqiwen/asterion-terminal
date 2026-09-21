import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";

type RecordValue = {
  run_id: string; created_at: number; selection: { reason: string; source_run: string; start: string; end: string };
  evidence: { research: { request: { start: string; end: string; parameters: unknown }; summary: Record<string, unknown> }; warmup_start: string; warmup_end: string; evaluation_start: string; evaluation_end: string; warmup_bars: number };
  validation: { state: string; result: Record<string, unknown> | null; error: string | null };
};
export function Validation({ api, connected, ready, identifier, items, researchEnd, base, onOpen }: {
  api: RequestClient; connected: boolean; ready: boolean; identifier: string;
  items: { id: string; state: string; parameters: unknown }[]; researchEnd: string;
  base: Record<string, unknown>; onOpen: (id: string) => void;
}) {
  const [record, setRecord] = useState<RecordValue | null>(null);
  const [source, setSource] = useState("");
  const [reason, setReason] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    let live = true;
    if (!connected) return;
    const refresh = () => api.request<RecordValue | null>(`/research/experiments/${identifier}/validation`)
      .then((value) => { if (live) setRecord(value); }).catch((e) => { if (live) setError(String(e)); });
    void refresh(); const timer = setInterval(() => void refresh(), 3000);
    return () => { live = false; clearInterval(timer); };
  }, [api, connected, identifier]);
  async function submit() {
    setBusy(true); setError("");
    try { setRecord(await api.request<RecordValue>(`/research/experiments/${identifier}/validation`, {
      source_run: source, reason, start: base.start, end: base.end,
      coverage_report_id: base.coverage_report_id, coverage_policy: base.coverage_policy, coverage_note: base.coverage_note,
    })); } catch (e) { setError(String(e)); } finally { setBusy(false); }
  }
  async function cancel() {
    setBusy(true); setError("");
    try { setRecord(await api.request<RecordValue>(`/research/experiments/${identifier}/validation/cancel`, {})); }
    catch (e) { setError(String(e)); } finally { setBusy(false); }
  }
  async function download() {
    setBusy(true); setError("");
    try {
      const value = await api.request<{ filename: string; content_base64: string }>(`/research/experiments/${identifier}/validation/export`);
      const bytes = Uint8Array.from(atob(value.content_base64), (c) => c.charCodeAt(0));
      const url = URL.createObjectURL(new Blob([bytes], { type: "application/zip" }));
      const link = document.createElement("a"); link.href = url; link.download = value.filename; link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (e) { setError(String(e)); } finally { setBusy(false); }
  }
  return <section aria-label="后续区间验证"><h3>后续区间验证</h3>
    <p>选择一个完成的组合，冻结后只在后续日期验证。独立资金、空仓开始，验证区间开头仅预热；不继承研究期仓位。不保证数据此前未被查看。</p>
    {error && <p role="alert">{error}</p>}
    {!record ? <fieldset disabled={!connected || busy}>
      <p>先在上方回测配置选择晚于 {researchEnd} 的日期并核对覆盖。这里只采用上方日期与覆盖设置；参数、策略、资金和规则固定为下方所选研究运行。</p>
      <p>待验证区间：{String(base.start)} — {String(base.end)}</p>
      <label>冻结组合<select aria-label="冻结组合" value={source} onChange={(e) => setSource(e.target.value)}><option value="">选择已完成组合</option>
        {items.filter((i) => i.state === "SUCCEEDED").map((i) => <option key={i.id} value={i.id}>{JSON.stringify(i.parameters)}</option>)}
      </select></label>
      <label>选择依据<textarea maxLength={1000} value={reason} onChange={(e) => setReason(e.target.value)} /></label>
      <button type="button" disabled={!ready || !source || !reason.trim() || items.some((i) => ["QUEUED", "RUNNING"].includes(i.state)) || String(base.start) <= researchEnd} onClick={() => void submit()}>冻结选择并验证</button>
      <p>每个实验只能冻结一次。请先完成或取消剩余研究组合；查看验证结果后不能覆盖原选择。</p>
    </fieldset> : <>
      <p>选择依据：{record.selection.reason}</p><p>固定参数：{JSON.stringify(record.evidence.research.request.parameters)}</p>
      <p>研究：{record.evidence.research.request.start} — {record.evidence.research.request.end}</p>
      <p>验证预热：{record.evidence.warmup_start} — {record.evidence.warmup_end}（{record.evidence.warmup_bars} 根，不交易）</p>
      <p>验证交易统计区间：{record.evidence.evaluation_start} — {record.evidence.evaluation_end}</p>
      <p>验证状态：{({ QUEUED: "排队中", RUNNING: "运行中", SUCCEEDED: "已完成", FAILED: "失败", CANCELLED: "已取消" } as Record<string, string>)[record.validation.state]} {record.validation.error}</p>
      <table className="data-table" aria-label="研究与验证对比"><thead><tr><th>指标</th><th>研究区间</th><th>验证区间</th></tr></thead><tbody>
        {[["net_profit", "净收益"], ["return_rate", "收益率"], ["max_drawdown", "最大回撤"], ["fees", "费用"], ["fill_count", "成交次数"]].map(([key, label]) => <tr key={key}><td>{label}</td>
          {[record.evidence.research.summary, record.validation.state === "SUCCEEDED" ? record.validation.result : null].map((summary, i) => <td key={i}>{summary?.[key] == null ? "—" : ["return_rate", "max_drawdown"].includes(key) ? `${(Number(summary[key]) * 100).toFixed(2)}%` : String(summary[key])}</td>)}
        </tr>)}
      </tbody></table>
      <button type="button" onClick={() => onOpen(record.selection.source_run)}>查看研究运行</button>
      <button type="button" onClick={() => onOpen(record.run_id)}>查看验证运行</button>
      <button type="button" disabled={busy || !connected || !["QUEUED", "RUNNING"].includes(record.validation.state)} onClick={() => void cancel()}>取消验证</button>
      <button type="button" disabled={busy || !connected || record.validation.state !== "SUCCEEDED"} onClick={() => void download()}>导出完整复现资料</button>
    </>}
  </section>;
}
