import { translate, type MessageValues } from "../../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useState, useEffect, type ReactNode } from "react";
type Navigation = {
  id: string;
  title: string;
  icon?: string;
  active: boolean;
  disabled?: boolean;
  shortcut?: string;
  select: () => void;
};
/** Product-independent chrome. Features supply content through named slots. */
export function Workbench({
  title,
  navigation,
  sections,
  rail,
  toolbar,
  notices,
  body,
  dock,
  status,
}: {
  title: string;
  navigation: readonly Navigation[];
  sections: readonly Navigation[];
  rail: ReactNode;
  toolbar: ReactNode;
  notices: ReactNode;
  body: ReactNode;
  dock: ReactNode;
  status: ReactNode;
}) {
  const [collapsed, setCollapsed] = useState(false);
  const activeId = navigation.find(item => item.active)?.id;
  const [opened, setOpened] = useState<string[]>(activeId ? [activeId] : []);
  useEffect(() => {
    if (activeId) setOpened(ids => (ids.includes(activeId) ? ids : [...ids, activeId]));
  }, [activeId]);
  return (
    <div className="terminal">
      <aside className={`activity-rail ${collapsed ? "collapsed" : "expanded"}`}>
        <div className="rail-brand" title="Asterion Terminal">
          ✧
        </div>
        <button
          className="rail-toggle"
          aria-label={collapsed ? t("展开导航") : t("收起导航")}
          onClick={() => setCollapsed(!collapsed)}
        >
          {collapsed ? "»" : "«"}
        </button>
        <nav aria-label={t("业务工作区")} className="workspace-tabs">
          {navigation.map(item => (
            <button
              key={item.id}
              disabled={item.disabled}
              aria-current={item.active ? "page" : undefined}
              className={item.active ? "active" : ""}
              onClick={item.select}
              title={`${item.title}${item.shortcut ? ` · ${item.shortcut}` : ""}`}
              aria-label={item.title}
            >
              <span aria-hidden="true">{item.icon}</span>
              <span className="nav-label">{item.title}</span>
            </button>
          ))}
        </nav>
        <div className="rail-actions">{rail}</div>
      </aside>
      <div className="terminal-content">
        <div className="open-workspaces" role="tablist" aria-label={t("已打开工作区")}>
          {opened.map(id => {
            const item = navigation.find(n => n.id === id);
            if (!item) return null;
            return (
              <div className="open-workspace" key={id}>
                <button role="tab" aria-selected={item.active} onClick={item.select}>
                  {item.title}
                </button>
                {opened.length > 1 && (
                  <button
                    aria-label={t("关闭{p0}标签", { p0: item.title })}
                    onClick={() => {
                      const remaining = opened.filter(i => i !== id);
                      setOpened(remaining);
                      if (item.active) navigation.find(n => n.id === remaining.at(-1))?.select();
                    }}
                  >
                    ×
                  </button>
                )}
              </div>
            );
          })}
        </div>
        <div className="workspace-toolbar">
          <h1>{title}</h1>
          {sections.length > 0 && <span className="toolbar-divider" />}
          <nav className="section-tabs" aria-label={t("业务内分类")}>
            {sections.map(item => (
              <button
                key={item.id}
                className={item.active ? "active" : ""}
                aria-current={item.active ? "page" : undefined}
                onClick={item.select}
              >
                <span className="nav-label">{item.title}</span>
              </button>
            ))}
          </nav>
          {toolbar}
        </div>
        {notices}
        {body}
        {dock}
        <footer className="status-bar">{status}</footer>
      </div>
    </div>
  );
}
