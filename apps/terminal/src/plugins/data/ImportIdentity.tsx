import { useEffect, useRef, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";

type Identity = components["schemas"]["ImportIdentity"];
type Release = components["schemas"]["ReferenceRelease"];
type Summary = components["schemas"]["ReferenceSummary"];

export function ImportIdentityPicker({ api, disabled, onChange }: {
  api: RequestClient;
  disabled: boolean;
  onChange: (value: Identity | null) => void;
}) {
  const [releases, setReleases] = useState<Summary[]>([]);
  const [release, setRelease] = useState<Release | null>(null);
  const [selected, setSelected] = useState("");
  const [rows, setRows] = useState([{ contract: "", mapping: "" }]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  async function refresh() {
    try { setReleases(await api.request<Summary[]>("/reference/releases?limit=100")); setError(""); }
    catch (e) { setError(String(e)); }
  }
  useEffect(() => { if (!disabled) void refresh(); }, [api, disabled]);
  function update(next: typeof rows, evidence = release) {
    setRows(next);
    if (!evidence || next.some(row => !row.contract.trim() || !row.mapping)) {
      onChange(null); return;
    }
    onChange({
      catalog_id: evidence.id, catalog: evidence.catalog,
      information_at: new Date().toISOString(),
      bindings: next.map(row => ({ contract: row.contract.trim(), ...JSON.parse(row.mapping) })),
    });
  }
  async function select(id: string) {
    const request = ++generation.current;
    setSelected(id); setRelease(null); onChange(null); setError("");
    const empty = [{ contract: "", mapping: "" }]; setRows(empty);
    if (!id) return;
    setLoading(true);
    try {
      const result = await api.request<Release>(`/reference/releases/${id}`);
      if (generation.current === request) setRelease(result);
    } catch (e) { if (generation.current === request) setError(String(e)); }
    finally { if (generation.current === request) setLoading(false); }
  }
  const mappings = release ? [...new Set(release.catalog.symbols.map(m => JSON.stringify({ source: m.source, symbol: m.symbol })))] : [];
  return <fieldset disabled={disabled || loading} className="import-options">
    <legend>合约身份</legend>
    <label>固定合约目录<select aria-label="导入合约目录" value={selected} onChange={e => void select(e.target.value)}>
      <option value="">请选择已发布目录</option>
      {releases.map(item => <option key={item.id} value={item.id}>{item.sources.join(" / ")} · {item.contracts} 合约 · {item.id.slice(0, 12)}</option>)}
    </select></label>
    <button type="button" onClick={() => void refresh()}>刷新目录</button>
    {rows.map((row, index) => <div className="import-options" key={index}>
      <label>文件合约代码<input aria-label={`文件合约代码 ${index + 1}`} value={row.contract} placeholder="SHFE.rb2610" onChange={e => update(rows.map((r, i) => i === index ? { ...r, contract: e.target.value } : r))} /></label>
      <label>目录来源代码<select aria-label={`目录来源代码 ${index + 1}`} value={row.mapping} onChange={e => update(rows.map((r, i) => i === index ? { ...r, mapping: e.target.value } : r))}>
        <option value="">请选择明确对应的来源代码</option>
        {mappings.map(value => { const m = JSON.parse(value); return <option key={value} value={value}>{m.source} · {m.symbol}</option>; })}
      </select></label>
      {rows.length > 1 && <button type="button" onClick={() => update(rows.filter((_, i) => i !== index))}>移除映射</button>}
    </div>)}
    <button type="button" onClick={() => update([...rows, { contract: "", mapping: "" }])}>添加合约映射</button>
    <p className="settings-note">先在合约资料中发布目录。每个文件代码须明确关联；预览按每行交易日检查生命周期。目录及映射随本次导入固定保存。</p>
    {error && <div role="alert" className="alert">{error}</div>}
  </fieldset>;
}
