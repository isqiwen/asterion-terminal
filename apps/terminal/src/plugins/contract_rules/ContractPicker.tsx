import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";

type Contract = components["schemas"]["Contract"];
type Release = components["schemas"]["ReferenceRelease"];
type Summary = components["schemas"]["ReferenceSummary"];

export function ContractPicker({ api, value, onChange }: {
  api: RequestClient;
  value: Contract | null;
  onChange: (value: Contract) => void;
}) {
  const [releases, setReleases] = useState<Summary[]>([]);
  const [selected, setSelected] = useState("");
  const [release, setRelease] = useState<Release | null>(null);
  const [error, setError] = useState("");
  useEffect(() => {
    let live = true;
    api.request<Summary[]>("/reference/releases?limit=100")
      .then(items => { if (live) setReleases(items); })
      .catch(e => { if (live) setError(String(e)); });
    return () => { live = false; };
  }, [api]);
  useEffect(() => {
    let live = true;
    setRelease(null); setError("");
    if (selected) api.request<Release>(`/reference/releases/${encodeURIComponent(selected)}`)
      .then(item => { if (live) setRelease(item); })
      .catch(e => { if (live) setError(String(e)); });
    return () => { live = false; };
  }, [api, selected]);
  return <div>
    <label>规则合约目录<select aria-label="规则合约目录" value={selected} onChange={e => setSelected(e.target.value)}>
      <option value="">选择已发布的合约目录</option>
      {releases.map(item => <option key={item.id} value={item.id}>{item.sources.join(" / ")} · {item.contracts} 合约 · {item.id.slice(0, 12)}</option>)}
    </select></label>
    <label>合约目录 ID<input value={selected} onChange={e => setSelected(e.target.value)} placeholder="可直接填写已发布目录 ID" /></label>
    <label>规则合约<select aria-label="规则合约" value={value?.id ?? ""} onChange={e => {
      const actual = release?.catalog.contracts.find(item => item.id === e.target.value);
      if (actual) onChange(actual);
    }}>
      <option value="">选择实际合约及上市生命周期</option>
      {value && !release?.catalog.contracts.some(item => item.id === value.id) && <option value={value.id}>{value.id} · 已固定</option>}
      {release?.catalog.contracts.map(item => <option key={item.id} value={item.id}>{item.id} · {item.listed_on}—{item.last_trade_on}</option>)}
    </select></label>
    {value && <p>交割月 {value.delivery_month} · 上市 {value.listed_on} · 最后交易日 {value.last_trade_on}</p>}
    <p>合约代码可能重复使用；规则固定完整合约身份。没有目录时，请先在合约资料中发布。</p>
    {error && <p role="alert">{error}</p>}
  </div>;
}
