import { useEffect, useState } from "react";
import { BatchSync } from "./BatchSync";
import { StartCalculation } from "./StartCalculation";
import type { components } from "@asterion/api-types/schema";
import type { RequestClient } from "@asterion/runtime-client/requests";

type Computed = components["schemas"]["ComputedVersion"];
type Report = components["schemas"]["RoleVersion"];
const reasons = {
  initial: "初次选择", retained: "保持", confirming: "等待连续确认", switched: "切换",
  expired_before_window: "观测窗口前已到期", listed_after_window: "观测窗口后才上市",
  not_listed: "尚未上市", expired: "已到期", expires_before_effective: "生效前到期", no_trade: "无成交",
};

export function RoleDiagnostics({ api, connected, active, productId = "", role }: {
  api: RequestClient; connected: boolean; active: boolean; productId?: string; role?: "main" | "secondary";
}) {
  const [mode, setMode] = useState<"computed" | "provider">("computed");
  const [offset, setOffset] = useState(0);
  const [refresh, setRefresh] = useState(0);
  const [records, setRecords] = useState<(Computed | Report)[]>([]);
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const [creating, setCreating] = useState(false);
  useEffect(() => {
    let live = true;
    setRecords([]); setSelected(""); setError(""); setLoading(false);
    if (!active || !connected) return;
    setLoading(true);
    const path = mode === "computed" ? "/contract-roles/computed" : "/contract-roles";
    api.request<(Computed | Report)[]>(`${path}?limit=20&offset=${offset}&product_id=${encodeURIComponent(productId)}`)
      .then((rows) => { if (live) { setRecords(rows); setSelected(rows[0]?.id || ""); } })
      .catch((reason: unknown) => { if (live) setError(reason instanceof Error ? reason.message : String(reason)); })
      .finally(() => { if (live) setLoading(false); });
    return () => { live = false; };
  }, [api, api.key, connected, active, mode, offset, refresh, productId]);
  const record = records.find((value) => value.id === selected);
  return <section className="panel-scroll" aria-label="合约角色诊断">
    <div className="panel-heading">
      <button disabled={creating} className={mode === "computed" ? "pressed" : ""} onClick={() => { setMode("computed"); setOffset(0); }}>计算角色</button>
      <button disabled={creating} className={mode === "provider" ? "pressed" : ""} onClick={() => { setMode("provider"); setOffset(0); }}>供应商报告</button>
      <span className="panel-spacer" />
      {mode === "computed" && !productId && <button disabled={!connected || loading || creating} onClick={() => setCreating(true)}>首次计算</button>}
      <button disabled={!connected || loading || creating} onClick={() => setRefresh((value) => value + 1)}>刷新</button>
    </div>
    <p className="panel-footnote">角色映射用于研究和诊断，不授权交易。供应商报告与自行计算分别保留证据。</p>
    {!connected && <p role="status">本机服务未连接</p>}
    {loading && <p role="status">正在读取角色版本…</p>}
    {error && <p className="notice" role="alert">{error}</p>}
    {creating && <StartCalculation key={api.key} api={api} enabled={connected && active} onCancel={() => setCreating(false)} onPublished={value => { setRecords(rows => [value, ...rows.filter(r => r.id !== value.id)]); setSelected(value.id); setMode("computed"); setCreating(false); }} />}
    {connected && !creating && !loading && !error && !records.length && <p className="panel-footnote">{mode === "computed" ? (productId ? "该品种暂无计算角色，可在合约角色页首次计算。" : "暂无计算角色。点击“首次计算”，选择固定资料与日线并预览发布。") : "暂无供应商角色报告。"}</p>}
    {!!records.length && <label>角色版本 <select aria-label="角色版本" value={selected} onChange={(event) => setSelected(event.target.value)}>
      {records.map((value) => <option key={value.id} value={value.id}>{value.spec.origin === "computed" ? value.spec.request.product_id : value.spec.product_id} · {value.id.slice(0, 12)}</option>)}
    </select></label>}
    {record && <div>
      <p className="panel-footnote">版本：<code>{record.id}</code></p>
      {"published_at" in record ? <><BatchSync key={`${api.key}:${record.id}`} api={api} version={record.id} enabled={connected && active} /><ComputedDetails record={record} role={role} /></> : <ReportDetails record={record} role={role} />}
    </div>}
    <div className="panel-heading">
      <button disabled={offset === 0 || loading || creating} onClick={() => setOffset((value) => Math.max(0, value - 20))}>上一页</button>
      <small>第 {offset / 20 + 1} 页 · 每页 20 项</small>
      <button disabled={records.length < 20 || loading || creating} onClick={() => setOffset((value) => value + 20)}>下一页</button>
    </div>
  </section>;
}

function ComputedDetails({ record, role }: { record: Computed; role?: "main" | "secondary" }) {
  const { spec } = record;
  return <>
    <p className="panel-footnote">候选范围：{spec.candidates.coverage === "explicit_subset" ? "手动指定子集" : "固定来源品种目录"} · 存续候选 {spec.candidates.included.length} 个 · 不证明交易所全市场完备</p>
    <p className="panel-footnote">可知依据：本机观测；未认证历史公布时间。发布于 {record.published_at}</p>
    <p className="panel-footnote">规则：{spec.request.policy.metric === "volume" ? "成交量" : "持仓量"} · 阈值 {spec.request.policy.switch_margin} · 连续确认 {spec.request.policy.confirmations} 次</p>
    <table className="data-table"><thead><tr><th>观测交易日</th><th>可知时间</th><th>生效交易日 / 开盘</th>{role !== "secondary" && <th>主力</th>}{role !== "main" && <th>次主力</th>}<th>决定</th></tr></thead>
      <tbody>{spec.result.decisions.map((row) => <tr key={row.observation_day}>
        <td>{row.observation_day}</td><td>{row.available_at}</td><td>{row.effective_day}<br />{row.effective_start}</td>{role !== "secondary" && <td>{row.main}</td>}{role !== "main" && <td>{row.secondary}</td>}<td>{reasons[row.reason]} · {row.confirmation_count}</td>
      </tr>)}</tbody></table>
    <details><summary>候选与排除依据</summary>
      <p className="panel-footnote">固定合约资料：{spec.request.contracts_version_id}</p>
      <ul>{spec.candidates.included.map((id) => <li key={id}>{id}</li>)}</ul>
      {spec.candidates.excluded.map((row) => <p className="panel-footnote" key={row.contract_id}>{row.symbol} · {row.contract_id} · {reasons[row.reason]}</p>)}
      {spec.result.decisions.flatMap((day) => day.excluded.map((row) => <p className="panel-footnote" key={`${day.observation_day}:${row.contract_id}`}>{day.observation_day} · {row.contract_id} · {reasons[row.reason]}</p>))}
    </details>
    <details><summary>固定输入与算法证据</summary><pre>{JSON.stringify({ inputs: spec.request.daily_inputs, policy: spec.request.policy, algorithm: spec.artifact.algorithm, checksum: spec.artifact.checksum, input_digest: spec.result.input_digest }, null, 2)}</pre></details>
  </>;
}

function ReportDetails({ record, role }: { record: Report; role?: "main" | "secondary" }) {
  return <>
    <p className="panel-footnote">来源：{record.spec.source} · 固定版本 {record.spec.source_version}</p>
    <p className="panel-footnote">未知公布时间的报告只支持明确声明的回顾分析，不能作为当时已知信号。</p>
    <table className="data-table"><thead><tr><th>交易日</th><th>角色</th><th>实际合约</th><th>公布时间</th></tr></thead>
      <tbody>{record.spec.reports.filter(row => !role || row.role === role).map((row) => <tr key={`${row.trading_day}:${row.role}`}><td>{row.trading_day}</td><td>{row.role === "main" ? "主力" : "次主力"}</td><td>{row.contract_id}</td><td>{row.available_at ?? "未知"}</td></tr>)}</tbody></table>
  </>;
}
