import { Validation } from "./Validation";
import { useEffect, useRef, useState } from "react";
import type { RequestClient } from "../../api/requests";
import { ParameterFields, parameterError, type ParameterDefinition, type ParameterValues } from "../../components/ParameterFields";
import type { StrategyRef } from "./StrategyPicker";

const states: Record<string, string> = { QUEUED: "排队中", RUNNING: "运行中", SUCCEEDED: "已完成", FAILED: "失败", CANCELLED: "已取消" };
type Experiment = { id: string; name: string; created_at: number; runs: string[] };
type Detail = Experiment & { spec: { base: { end: string } }; items: { id: string; state: string; parameters: ParameterValues; error: string | null; result: Record<string, unknown> | null }[] };
export function Experiments({ api, connected, ready, base, onOpen }: {
  api: RequestClient; connected: boolean; ready: boolean;
  base: Record<string, unknown> & { strategy: StrategyRef | null; parameters: ParameterValues };
  onOpen: (id: string) => void;
}) {
  const [fields, setFields] = useState<ParameterDefinition[]>([]);
  const [grid, setGrid] = useState<Record<string, ParameterValues[string][]>>({});
  const [name, setName] = useState("");
  const [list, setList] = useState<Experiment[]>([]);
  const [selected, setSelected] = useState("");
  const [detail, setDetail] = useState<Detail | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const command = useRef({ signature: "", id: "" });
  useEffect(() => {
    let live = true;
    setFields([]); setGrid({});
    if (connected && base.strategy) api.request<{ identity: StrategyRef; parameters: ParameterDefinition[] }[]>("/research/strategies")
      .then((items) => { if (live) setFields(items.find((item) => item.identity.digest === base.strategy?.digest && item.identity.id === base.strategy.id)?.parameters ?? []); })
      .catch((e) => { if (live) setError(String(e)); });
    return () => { live = false; };
  }, [api, connected, base.strategy?.id, base.strategy?.digest]);
  useEffect(() => {
    if (!connected) return;
    let live = true;
    const refresh = async () => {
      try {
        const response = await api.request<{ items: Experiment[] }>("/research/experiments");
        if (!Array.isArray(response.items)) throw new Error("实验列表响应无效");
        if (live) setList(response.items);
        if (selected) {
          const item = await api.request<Detail>(`/research/experiments/${selected}`);
          if (live) setDetail(item);
        }
      } catch (e) { if (live) setError(String(e)); }
    };
    void refresh(); const timer = setInterval(() => void refresh(), 3000);
    return () => { live = false; clearInterval(timer); };
  }, [api, connected, selected]);
  const count = Object.keys(grid).length ? Object.values(grid).reduce((n, items) => n * items.length, 1) : 0;
  const invalid = Object.entries(grid).some(([key, candidates]) => {
    const field = fields.find((p) => p.key === key);
    return !field || candidates.length === 0 || new Set(candidates.map((v) => JSON.stringify(v))).size !== candidates.length || candidates.some((value) => parameterError([field], { [key]: value }));
  });
  async function submit() {
    const input = { name, base, grid };
    const signature = JSON.stringify(input);
    if (command.current.signature !== signature) command.current = { signature, id: crypto.randomUUID() };
    setBusy(true); setError("");
    try {
      const item = await api.request<Detail>("/research/experiments", { ...input, base: { ...base, command_id: command.current.id } });
      setDetail(item); setSelected(item.id);
      setList((items) => [item, ...items.filter((i) => i.id !== item.id)]);
    } catch (e) { setError(`${String(e)}。可用相同输入重试，服务器不会重复创建实验。`); }
    finally { setBusy(false); }
  }
  async function cancel() {
    if (!detail) return;
    setBusy(true); setError("");
    try { setDetail(await api.request<Detail>(`/research/experiments/${detail.id}/cancel`, {})); }
    catch (e) { setError(String(e)); }
    finally { setBusy(false); }
  }
  return <section aria-label="参数实验">
    <details><summary>参数批量实验</summary>
      <p>沿用上方固定数据、规则和研究假设。选择要变化的参数并添加候选值，最多 4 个参数、32 个组合。</p>
      <fieldset disabled={!connected || busy}>
        <label>实验名称<input maxLength={80} value={name} onChange={(e) => setName(e.target.value)} /></label>
        {fields.map((p) => <div key={p.key}>
          <label><input type="checkbox" checked={p.key in grid}
            disabled={!(p.key in grid) && Object.keys(grid).length >= 4}
            onChange={(e) => setGrid((old) => { const next = { ...old }; if (e.target.checked) next[p.key] = [base.parameters[p.key]]; else delete next[p.key]; return next; })} />变化：{p.label}</label>
          {grid[p.key]?.map((value, index) => <div key={index} aria-label={`${p.label}候选 ${index + 1}`}>
            <ParameterFields fields={[p]} values={{ [p.key]: value }} onChange={(values) => setGrid((old) => ({ ...old, [p.key]: old[p.key].map((v, i) => i === index ? values[p.key] : v) }))} />
            <button type="button" onClick={() => setGrid((old) => ({ ...old, [p.key]: old[p.key].filter((_, i) => i !== index) }))}>移除此候选</button>
          </div>)}
          {p.key in grid && <button type="button" disabled={grid[p.key].length >= 32} onClick={() => setGrid((old) => ({ ...old, [p.key]: [...old[p.key], p.default] }))}>添加{p.label}候选</button>}
        </div>)}
        <p>{count} 个组合{invalid && " · 候选值缺失、重复或不符合参数声明"}</p>
        <button type="button" disabled={!ready || !name.trim() || count < 2 || count > 32 || invalid} onClick={() => void submit()}>提交参数实验</button>
        {!ready && <p>请先完成上方回测配置并确认研究假设。</p>}
      </fieldset>
    </details>
    {error && <p role="alert">{error}</p>}
    <label>实验记录<select value={selected} onChange={(e) => { setSelected(e.target.value); setDetail(null); }}>
      <option value="">选择实验</option>{list.map((item) => <option key={item.id} value={item.id}>{item.name} · {item.runs.length} 个组合</option>)}
    </select></label>
    {detail && <>
      <Validation key={detail.id} api={api} connected={connected} ready={ready} identifier={detail.id} items={detail.items} researchEnd={detail.spec.base.end} base={base} onOpen={onOpen} />
      <h3>{detail.name}</h3><p>自动刷新 · 样本内参数对比，不代表样本外表现。失败和取消项仍保留。</p>
      <button type="button" disabled={busy || !connected || !detail.items.some((i) => ["QUEUED", "RUNNING"].includes(i.state))} onClick={() => void cancel()}>取消未完成组合</button>
      <table className="data-table" aria-label="实验结果对比"><thead><tr><th>参数</th><th>状态</th><th>净收益</th><th>最大回撤</th><th>费用</th><th>成交数</th><th>操作</th></tr></thead>
        <tbody>{detail.items.map((item) => <tr key={item.id}><td>{JSON.stringify(item.parameters)}</td><td title={item.error ?? undefined}>{states[item.state] ?? item.state}{item.error && <small>{item.error}</small>}</td>
          {["net_profit", "max_drawdown", "fees", "fill_count"].map((key) => <td key={key}>{item.state === "SUCCEEDED" ? (key === "max_drawdown" && item.result?.[key] != null ? `${(Number(item.result[key]) * 100).toFixed(2)}%` : String(item.result?.[key] ?? "—")) : "—"}</td>)}
          <td><button type="button" onClick={() => onOpen(item.id)}>查看运行</button></td></tr>)}</tbody></table>
    </>}
  </section>;
}
