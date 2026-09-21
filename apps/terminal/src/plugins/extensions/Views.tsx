import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import { DeclarativeTable, type TableDefinition } from "../../extensions/DeclarativeTable";

type View = { plugin_id: string; digest: string; view: TableDefinition };
type Row = Record<string, string | number | boolean | null>;

export function Views({ api }: { api: RequestClient }) {
  const [views, setViews] = useState<View[]>([]);
  const [selected, setSelected] = useState("");
  const [revision, setRevision] = useState(0);
  const [rows, setRows] = useState<Row[]>([]);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const view = views.find((entry) => entry.plugin_id === selected);
  useEffect(() => {
    let active = true;
    api.request<{ items: View[] }>("/extensions/views").then((result) => {
      if (active) {
        setViews(result.items);
        setSelected((current) => current || result.items[0]?.plugin_id || "");
      }
    }, (reason) => { if (active) setError(String(reason)); });
    return () => { active = false; };
  }, [api, revision]);
  useEffect(() => {
    let active = true;
    setRows([]); setError("");
    if (!view) { setBusy(false); return; }
    setBusy(true);
    api.request<{ rows: Row[] }>(`/extensions/${view.plugin_id}/view`, { digest: view.digest }).then(
      (value) => { if (active) setRows(value.rows); },
      (reason) => { if (active) setError(String(reason)); },
    ).finally(() => { if (active) setBusy(false); });
    return () => { active = false; };
  }, [api, view, revision]);
  return <section className="data-panel">
    <div className="panel-toolbar">
      <label>插件视图 <select aria-label="插件视图" value={selected} onChange={(event) => setSelected(event.target.value)}>
        {!views.length && <option value="">暂无可用视图</option>}
        {selected && !view && <option value={selected}>插件不可用</option>}
        {views.map((entry) => <option key={entry.plugin_id} value={entry.plugin_id}>{entry.view.title}</option>)}
      </select></label>{" "}
      <button disabled={busy} onClick={() => setRevision((value) => value + 1)}>刷新</button>
    </div>
    {error && <p role="alert" className="alert">{error}</p>}
    {busy && <p>正在读取插件视图…</p>}
    {!view && <p className="empty">插件视图不可用。可在设置中安装并启用插件，然后刷新。</p>}
    {view && !busy && <DeclarativeTable definition={view.view} rows={rows} />}
  </section>;
}
