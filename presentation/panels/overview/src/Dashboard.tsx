import { useState, useEffect, useRef, type ReactNode } from "react";
import type { PanelContext } from "./context";
import { parseDashboardLayout, type DashboardLayout } from "./layout";
import "./dashboard.css";
function CatalogDialog({ children, close, title = "添加组件", drawer = false }: { children: ReactNode; close: () => void; title?: string; drawer?: boolean }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const dialog = ref.current;
    const previous = document.activeElement;
    dialog?.showModal();
    return () => {
      dialog?.close();
      if (previous instanceof HTMLElement) requestAnimationFrame(() => previous.isConnected && previous.focus());
    };
  }, []);
  return <dialog ref={ref} aria-label={title} className={`widget-catalog ${drawer ? "reminder-drawer" : ""}`} onCancel={e => { e.preventDefault(); close(); }}>{children}</dialog>;
}
export function Dashboard(context: PanelContext["overview"]) {
  const { widgets, storageKey } = context;
  const defaults = (): DashboardLayout => ({ version: 1, refreshSeconds: 0, compact: false, pulse: false,
    items: widgets.filter(w => w.defaultVisible).map(w => ({ id: w.id, width: w.width })) });
  const [initial] = useState(() => {
    try { const raw = localStorage.getItem(storageKey); return { layout: raw ? parseDashboardLayout(raw) : defaults(), error: "" }; }
    catch (e) { return { layout: defaults(), error: `布局读取失败：${String(e)}` }; }
  });
  const refresh = useRef(context.refresh);
  refresh.current = context.refresh;
  const [layout, setLayout] = useState(initial.layout);
  useEffect(() => {
    if (!layout.refreshSeconds) return;
    const timer = setInterval(() => refresh.current(), layout.refreshSeconds * 1000);
    return () => clearInterval(timer);
  }, [layout.refreshSeconds]);
  const [editing, setEditing] = useState(false);
  const [baseline, setBaseline] = useState(initial.layout);
  const [error, setError] = useState(initial.error);
  const [message, setMessage] = useState("");
  const [dirty, setDirty] = useState(false);
  const [reminders, setReminders] = useState(false);
  const [adding, setAdding] = useState(false);
  const [query, setQuery] = useState("");
  const [category, setCategory] = useState("");
  const [dragged, setDragged] = useState<string | null>(null);
  const change = (next: DashboardLayout) => { setLayout(next); setDirty(true); setMessage(""); };
  const move = (id: string, target: string) => {
    const items = [...layout.items]; const from = items.findIndex(i => i.id === id); const to = items.findIndex(i => i.id === target);
    if (from < 0 || to < 0 || from === to) return;
    items.splice(to, 0, items.splice(from, 1)[0]); change({ ...layout, items });
  };
  const save = () => {
    try { localStorage.setItem(storageKey, JSON.stringify(layout)); setDirty(false); setBaseline(layout); setEditing(false); setMessage("布局已保存"); }
    catch (e) { setError(`布局保存失败：${String(e)}`); }
  };
  const sideWidgets = widgets.filter(w => w.position === "aside");
  const reminderCount = sideWidgets.reduce((count, w) => count + (w.attentionCount?.(context) ?? 0), 0);
  function card(id: string, width: number, index: number, side = false): ReactNode {
    const widget = widgets.find(w => w.id === id);
    const title = widget?.title ?? id;
    const siblings = side ? [] : layout.items.filter(i => column(i) === column(layout.items[index]));
    const siblingIndex = siblings.findIndex(i => i.id === id);
    return <section className={`dashboard-tile widget-width-${width}`} aria-label={title} key={id}
      onDragOver={e => e.preventDefault()} onDrop={e => { e.preventDefault(); if (editing && dragged) move(dragged, id); setDragged(null); }}>
      <header draggable={editing && !side} onDragStart={() => setDragged(id)} onDragEnd={() => setDragged(null)}>
        <h2>{title}</h2><div className="widget-tools">
          {editing && !side && <>
            <button title="向前移动" aria-label={`向前移动${title}`} disabled={siblingIndex <= 0} onClick={() => move(id, siblings[siblingIndex - 1].id)}>←</button>
            <button title="向后移动" aria-label={`向后移动${title}`} disabled={siblingIndex < 0 || siblingIndex === siblings.length - 1} onClick={() => move(id, siblings[siblingIndex + 1].id)}>→</button>
            <select aria-label={`${title}位置`} value={layout.items[index].column ?? widget?.column ?? "full"} onChange={e => change({ ...layout, items: layout.items.map(i => i.id === id ? { ...i, column: e.target.value as "primary" | "secondary" | "full" } : i) })}><option value="primary">左栏</option><option value="secondary">右栏</option><option value="full">通栏</option></select>
            <button title="移除组件" aria-label={`移除${title}`} onClick={() => change({ ...layout, items: layout.items.filter(i => i.id !== id) })}>×</button>
          </>}
        </div>
      </header>
      {widget ? widget.render(context) : <div className="dashboard-empty">组件不可用；布局引用已保留。请检查插件是否启用。</div>}
    </section>;
  }
  const column = (item: DashboardLayout["items"][number]) => item.column ?? widgets.find(w => w.id === item.id)?.column ?? "full";
  const primary = layout.items.filter(i => column(i) === "primary");
  const secondary = layout.items.filter(i => column(i) === "secondary");
  const full = layout.items.filter(i => column(i) === "full");
  const renderItems = (items: DashboardLayout["items"]) => items.map(i => card(i.id, i.width, layout.items.indexOf(i)));
  return <div className={`dashboard ${layout.compact ? "dashboard-compact" : ""}`} aria-label="工作总览">
    <div className="dashboard-toolbar"><span>总览</span><span className="panel-spacer" />
      {editing && <button aria-pressed={layout.compact} onClick={() => change({ ...layout, compact: !layout.compact })}>紧凑</button>}
      <button aria-label={`提醒 ${reminderCount}`} title="查看异常与提醒" aria-haspopup="dialog" aria-expanded={reminders} onClick={() => setReminders(true)}><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" aria-hidden="true"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9M10 21h4" /></svg> 提醒{reminderCount > 0 && <span className="reminder-count">{reminderCount}</span>}</button>
      <button onClick={context.refresh}>刷新数据</button>
      {!editing && <button onClick={() => { setBaseline(layout); setEditing(true); }}>编辑布局</button>}
      {editing && <>
      <select aria-label="行情自动刷新" value={layout.refreshSeconds} onChange={e => change({ ...layout, refreshSeconds: Number(e.target.value) as 0 | 60 | 600 })}><option value={0}>行情自动刷新：关闭</option><option value={60}>每分钟</option><option value={600}>每 10 分钟</option></select>
      <button onClick={() => setAdding(true)}>添加组件</button>
      <button disabled={!!initial.error} onClick={save}>保存布局{dirty ? " *" : ""}</button>
      <button disabled={!!initial.error} onClick={() => { change(defaults()); setMessage("已恢复默认排列，保存后生效"); }}>重置布局</button>
      <button onClick={() => { setLayout(baseline); setDirty(false); setEditing(false); setMessage(""); }}>取消编辑</button>
      </>}

    </div>
    {error && <p role="alert" className="notice">{error}</p>}
    {message && <p role="status">{message}</p>}
    {context.catalogError && <p role="alert" className="notice">工作状态读取失败，以下为上次读取的记录。{context.catalogError}</p>}
    {reminderCount > 0 && <div role="alert" className="dashboard-attention"><span>{reminderCount} 项异常需要关注</span><button onClick={() => setReminders(true)}>查看异常</button></div>}
    <div className={`dashboard-columns ${layout.pulse ? "with-pulse" : ""}`}>
      <div className="dashboard-content">
        <div className={`dashboard-split ${primary.length && secondary.length ? "has-both" : ""}`}>
          {!!primary.length && <div className="dashboard-stack dashboard-primary">{renderItems(primary)}</div>}
          {!!secondary.length && <div className="dashboard-stack dashboard-secondary">{renderItems(secondary)}</div>}
        </div>
        {!!full.length && <div className="dashboard-grid dashboard-full">{renderItems(full)}</div>}
        {!layout.items.length && <div className="dashboard-empty">总览为空，可通过“编辑布局”添加组件。</div>}
      </div>
      {layout.pulse && <aside className="dashboard-pulse" aria-label="市场摘要栏">{sideWidgets.map(w => card(w.id, 1, 0, true))}</aside>}
    </div>
    {reminders && <CatalogDialog title="异常与提醒" drawer close={() => setReminders(false)}>
      <header><h2>异常与提醒</h2><button onClick={() => setReminders(false)}>关闭提醒</button></header>
      <div className="dashboard-controls"><button aria-pressed={layout.pulse} onClick={() => { change({ ...layout, pulse: !layout.pulse }); setReminders(false); }}>{layout.pulse ? "取消固定侧栏" : "固定为侧栏"}</button><small>按需查看；固定布局需另行保存。</small></div>
      {sideWidgets.map(w => <div key={w.id}>{w.render({ ...context, openTasks: () => { setReminders(false); context.openTasks(); } })}</div>)}
    </CatalogDialog>}
    {adding && <CatalogDialog close={() => setAdding(false)}>
      <header><h2>添加组件</h2><button onClick={() => setAdding(false)}>关闭</button></header>
      <div className="dashboard-controls"><input autoFocus aria-label="搜索组件" placeholder="搜索名称或用途" value={query} onChange={e => setQuery(e.target.value)} /><select aria-label="组件分类" value={category} onChange={e => setCategory(e.target.value)}><option value="">全部分类</option>{[...new Set(widgets.map(w => w.category))].map(c => <option key={c}>{c}</option>)}</select></div>
      <ul className="dashboard-list">{widgets.filter(w => w.position !== "aside" && (!category || w.category === category) && `${w.title} ${w.description}`.includes(query)).map(w => <li key={w.id}><div><strong>{w.title}</strong><button disabled={layout.items.some(i => i.id === w.id)} onClick={() => change({ ...layout, items: [...layout.items, { id: w.id, width: w.width }] })}>{layout.items.some(i => i.id === w.id) ? "已添加" : `添加${w.title}`}</button></div><p>{w.description}</p><small>{w.category}</small></li>)}</ul>
    </CatalogDialog>}
  </div>;
}
