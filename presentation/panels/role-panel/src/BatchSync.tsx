import { useEffect, useState } from "react";
import type { components } from "@asterion/api-types/schema";
import type { RequestClient } from "@asterion/runtime-client/requests";

type Plan = components["schemas"]["BatchPlan"];
type Workflow = components["schemas"]["WorkflowRecord"];
type Provider = components["schemas"]["ProviderStatus"];
type Job = components["schemas"]["Job"];
type Progress = Workflow & { dependencies: { id: string; symbol: string; state: string; version_id: string | null }[] };
const states: Record<string, string> = { QUEUED: "排队", RUNNING: "执行中", SUCCEEDED: "成功", FAILED: "失败", CANCELLED: "已取消" };
const message = (error: unknown) => error instanceof Error ? error.message : String(error);

export function BatchSync({ api, version, enabled }: { api: RequestClient; version: string; enabled: boolean }) {
  const [open, setOpen] = useState(false);
  const [plan, setPlan] = useState<Plan>();
  const [connections, setConnections] = useState<Provider[]>([]);
  const [connection, setConnection] = useState("");
  const [records, setRecords] = useState<Workflow[]>([]);
  const [selected, setSelected] = useState("");
  const [progress, setProgress] = useState<Progress>();
  const [job, setJob] = useState<Job>();
  const [error, setError] = useState("");
  const [statusError, setStatusError] = useState("");
  const [historyReady, setHistoryReady] = useState(false);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState(false);
  const [attempt, setAttempt] = useState<components["schemas"]["BatchContinuation"]>();
  const [reload, setReload] = useState(0);
  useEffect(() => {
    if (!open || !enabled) return;
    let live = true;
    setLoading(true); setHistoryReady(false); setError("");
    // History remains readable even when a source needed for a new plan is unavailable.
    void api.request<Workflow[]>(`/contract-roles/computed/${version}/sync-workflows`)
      .then((rows) => { if (live) { setHistoryReady(true); setRecords(rows); setSelected((id) => id || rows[0]?.id || ""); } })
      .catch((e) => { if (live) setError(message(e)); });
    void Promise.all([
      api.request<Plan>(`/contract-roles/computed/${version}/sync-plan`),
      api.request<Provider[]>("/data/providers"),
    ]).then(([next, providers]) => {
      if (!live) return;
      const choices = providers.filter((p) => (p.plugin_id ?? p.id) === next.provider && p.configured && p.lifecycle.state === "enabled");
      setPlan(next); setConnections(choices);
      setConnection((id) => choices.some((p) => p.id === id) ? id : choices[0]?.id || "");
    }).catch((e) => { if (live) { setPlan(undefined); setError(message(e)); } })
      .finally(() => { if (live) setLoading(false); });
    return () => { live = false; };
  }, [api, version, open, enabled, reload]);
  useEffect(() => {
    setProgress(undefined); setJob(undefined); setStatusError("");
    if (!open || !enabled || !selected) return;
    let live = true;
    let timer: ReturnType<typeof setTimeout>;
    async function poll() {
      try {
        const value = await api.request<Progress>(`/contract-roles/computed/sync-workflows/${selected}`);
        const nextJob = value.job_id ? await api.request<Job>(`/contract-roles/computed/tasks/${value.job_id}`) : undefined;
        if (!live) return;
        setProgress(value); setJob(nextJob); setStatusError("");
        if (nextJob && ["SUCCEEDED", "FAILED", "CANCELLED"].includes(nextJob.state)) return;
        if (!nextJob && (value.error || value.dependencies.some((d) => ["FAILED", "CANCELLED"].includes(d.state)))) return;
      } catch (e) { if (live) setStatusError(message(e)); }
      if (live) timer = setTimeout(() => void poll(), 3000);
    }
    void poll();
    return () => { live = false; clearTimeout(timer); };
  }, [api, selected, enabled, open, reload]);
  async function submit() {
    if (!plan || busy || !connection || !enabled || !historyReady) return;
    const provider = connections.find((p) => p.id === connection);
    if (!provider) return;
    const body = attempt ?? { command_id: `role-panel:${version}`, previous_version_id: version,
      trading_day: plan.trading_day, connection_id: provider.connection_id ?? null, explanation: "角色面板提交下一观测交易日全候选采集" };
    setAttempt(body); setBusy(true); setError("");
    try {
      const row = await api.request<Workflow>("/contract-roles/computed/sync-batches", body, 60000);
      setRecords((rows) => [row, ...rows.filter((r) => r.id !== row.id)]); setSelected(row.id);
    } catch (e) { setError(`${message(e)}。可重试原请求，命令和输入保持不变。`); }
    finally { setBusy(false); }
  }
  return <section aria-label="采集与续算">
    <button aria-expanded={open} onClick={() => setOpen(!open)}>采集与续算</button>
    {open && <>
      <p className="panel-footnote">按固定日历采集下一观测交易日，全部候选成功后自动续算。不会启用定时采集。</p>
      {loading && <p role="status">正在核验采集计划…</p>}
      {error && <p role="alert" className="notice">{error}</p>}
      {plan && <>
        <p>观测交易日：{plan.trading_day} · {plan.symbols.length} 个候选 · 来源 {plan.provider}</p>
        <details><summary>本次候选</summary><p>{plan.symbols.join("、")}</p></details>
        {!records.length && <><label>采集连接 <select aria-label="采集连接" value={connection} disabled={!enabled || busy || !!attempt || !!records.length} onChange={(e) => setConnection(e.target.value)}>
          {connections.map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
        </select></label>
        {!connections.length && <p className="notice">没有已配置且启用的同来源连接，请到数据源设置中配置。</p>}
        <button disabled={!enabled || loading || busy || !historyReady || !connection || !!records.length} title="一次提交全部候选，重复请求复用同一批任务" onClick={() => void submit()}>{busy ? "提交中…" : attempt ? "重试原请求" : "采集并续算"}</button></>}
      </>}
      <button disabled={!enabled || loading || busy} onClick={() => setReload((n) => n + 1)}>刷新采集进度</button>
      {!!records.length && <label>采集记录 <select aria-label="采集记录" value={selected} onChange={(e) => setSelected(e.target.value)}>
        {records.map((r) => <option key={r.id} value={r.id}>{r.request.trading_day} · {r.id.slice(0, 12)}</option>)}
      </select></label>}
      {statusError && <p className="notice" role="alert">进度读取失败：{statusError}</p>}
      {progress && <>
        <table className="data-table"><thead><tr><th>候选合约</th><th>同步状态</th><th>固定行情版本</th></tr></thead><tbody>
          {progress.dependencies.map((d) => <tr key={d.id}><td>{d.symbol}</td><td>{states[d.state] ?? d.state}</td><td title={d.version_id ?? undefined}>{d.version_id?.slice(0, 12) ?? "—"}</td></tr>)}
        </tbody></table>
        {progress.error && <p className="notice" role="alert">{progress.error}</p>}
        {progress.dependencies.some((d) => ["FAILED", "CANCELLED"].includes(d.state)) && <p className="notice">采集中断，未生成续算结果。重试采集后需重新登记对应任务；当前面板仅支持查看此记录。</p>}
        <p role="status">续算：{job ? states[job.state] ?? job.state : "等待全部候选同步完成"}</p>
        {job?.error && <p role="alert" className="notice">{job.error}</p>}
        {job?.result?.computed_version_id && <p>已发布角色版本：<code>{String(job.result.computed_version_id)}</code></p>}
      </>}
    </>}
  </section>;
}
