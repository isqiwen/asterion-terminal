import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";

export type TimeVersion = components["schemas"]["TimeVersion"];
export function TimePicker({ api, value, onChange, disabled = false }: { api: RequestClient; value: TimeVersion | null; onChange: (v: TimeVersion) => void; disabled?: boolean }) {
  const [items, setItems] = useState<TimeVersion[]>([]);
  const [editing, setEditing] = useState(false);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useEffect(() => { if (disabled) return; let live = true; api.request<TimeVersion[]>("/trading-time").then(v => { if (live) setItems(v); }).catch(e => { if (live) setError(String(e)); }); return () => { live = false; }; }, [api, disabled]);
  return <section aria-label="交易时间">
    <label>交易时间版本<select aria-label="交易时间版本" disabled={disabled || busy} value={value?.id ?? ""} onChange={e => { const v = items.find(i => i.id === e.target.value); if (v) onChange(v); }}>
      <option value="">选择品种时段与日历</option>
      {value && !items.some(i => i.id === value.id) && <option value={value.id}>{value.spec.title} · 固定版本</option>}
      {items.map(i => <option key={i.id} value={i.id}>{i.spec.exchange}.{i.spec.product} · {i.spec.title}</option>)}
    </select></label>
    <button type="button" disabled={disabled || busy} onClick={() => setEditing(!editing)}>配置交易时间</button>
    <label>导入时间规则文件<input type="file" accept=".json" disabled={disabled || busy} onChange={async e => { const f = e.target.files?.[0]; if (!f) return; setBusy(true); setError(""); try { if (f.size > 2_000_000) throw new Error("文件不能超过 2 MB"); const input = JSON.parse(await f.text()); const saved = await api.request<TimeVersion>("/trading-time", input.spec ?? input); setItems(v => [saved, ...v.filter(i => i.id !== saved.id)]); onChange(saved); } catch (err) { setError(String(err)); } finally { setBusy(false); } }} /></label>
    {editing && <TimeEditor api={api} onSaved={v => { setItems(items => [v, ...items.filter(i => i.id !== v.id)]); onChange(v); setEditing(false); }} />}
    {value && <button type="button" onClick={() => {const url=URL.createObjectURL(new Blob([JSON.stringify(value,null,2)],{type:"application/json"}));const a=document.createElement("a");a.href=url;a.download=`trading-time-${value.id.slice(0,12)}.json`;a.click();URL.revokeObjectURL(url);}}>导出时间版本</button>}
    {value && <p>{value.spec.exchange}.{value.spec.product} · {value.spec.timezone} · {value.spec.periods[0].start}—{value.spec.periods.at(-1)?.end}<br/>日历：{value.spec.calendar_source}<br/>夜盘：{value.spec.night_source}</p>}
    {error && <p role="alert">{error}</p>}
  </section>;
}

function TimeEditor({ api, onSaved }: { api: RequestClient; onSaved: (v: TimeVersion) => void }) {
  const [exchange, setExchange] = useState("SHFE");
  const [product, setProduct] = useState("");
  const [title, setTitle] = useState("");
  const [start, setStart] = useState(""); const [end, setEnd] = useState("");
  const [source, setSource] = useState(""); const [nightSource, setNightSource] = useState("");
  const [calendar, setCalendar] = useState("");
  const [day, setDay] = useState("09:00:00-10:15:00,10:30:00-11:30:00,13:30:00-15:00:00");
  const [night, setNight] = useState("");
  const [error, setError] = useState(""); const [busy, setBusy] = useState(false);
  async function save() { setBusy(true); setError(""); try {
    const slots = (text: string) => text.trim() ? text.split(",").map(v => { const [a,b] = v.trim().split("-"); return {start:a, end:b, end_offset:b <= a ? 1 : 0, phase:"continuous"}; }) : [];
    const days = calendar.trim().split(/\r?\n/).map(line => { const [date, open, night] = line.split(",").map(v => v.trim()); if (!["0","1"].includes(open) || !["0","1"].includes(night)) throw new Error("日历每行填写 日期,日盘开市0或1,当晚夜盘0或1"); return { date, is_open:open === "1", night_open:night === "1" }; });
    const v = await api.request<TimeVersion>("/trading-time", {schema_version:1, exchange, product, title, timezone:"Asia/Shanghai", calendar_source:source, night_source:nightSource, calendar:days, periods:[{start,end,source,day:slots(day),night:slots(night)}], exceptions:[]}); onSaved(v);
  } catch(e) { setError(String(e)); } finally { setBusy(false); } }
  return <fieldset disabled={busy}><legend>品种时段与完整日历</legend>
    <label>名称<input value={title} onChange={e => setTitle(e.target.value)} /></label>
    <label>交易所<select value={exchange} onChange={e => setExchange(e.target.value)}>{["SHFE","INE","DCE","CZCE","CFFEX","GFEX"].map(v => <option key={v}>{v}</option>)}</select></label>
    <label>品种代码<input value={product} onChange={e => setProduct(e.target.value.toUpperCase())} placeholder="例如 RB，不含合约月份" /></label>
    <label>时段生效起日<input type="date" value={start} onChange={e => setStart(e.target.value)} /></label><label>时段生效止日<input type="date" value={end} onChange={e => setEnd(e.target.value)} /></label>
    <label>日历与时段依据<input value={source} onChange={e => setSource(e.target.value)} placeholder="公告链接及生效范围" /></label>
    <label>夜盘停开依据<input value={nightSource} onChange={e => setNightSource(e.target.value)} placeholder="节假日安排、夜盘公告；无夜盘也需说明" /></label>
    <label>日盘时段<input value={day} onChange={e => setDay(e.target.value)} /></label>
    <label>夜盘时段<input value={night} onChange={e => setNight(e.target.value)} placeholder="如 21:00:00-02:30:00；无夜盘留空" /></label>
    <label>完整自然日日历<textarea rows={7} value={calendar} onChange={e => setCalendar(e.target.value)} placeholder="每行：YYYY-MM-DD,日盘开市0或1,当晚夜盘0或1" /></label>
    <p>须包含起日前的开市日，且不能省略周末或节假日。当晚夜盘按下一开市交易日的品种时段归属；节前是否开夜盘必须按公告填写。历史时段变化、临时停盘与集合竞价可通过时间规则文件的 periods / exceptions 明确定义。</p>
    <button type="button" disabled={busy} onClick={save}>{busy ? "保存中…" : "保存时间版本"}</button>{error && <p role="alert">{error}</p>}
  </fieldset>;
}
