import { BrandMark } from "../../ui/BrandMark";
import { translate, type MessageValues } from "../../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useState, useEffect, type ReactNode } from "react";
type Navigation = {
  id: string;
  title: string;
  icon?: ReactNode;
  active: boolean;
  disabled?: boolean;
  shortcut?: string;
  select: () => void;
};
/** Product-independent chrome. Features supply content through named slots. */
export function Workbench({
  title,
  showToolbar = true,
  navigation,
  rail,
  toolbar,
  notices,
  body,
  dock,
  status,
}: {
  title: string;
  showToolbar?: boolean;
  navigation: readonly Navigation[];
  rail: ReactNode;
  toolbar: ReactNode;
  notices: ReactNode;
  body: ReactNode;
  dock: ReactNode;
  status: ReactNode;
}) {
  const [collapsed, setCollapsed] = useState(false);
  useEffect(() => {
    const toggleNavigation = (event: KeyboardEvent) => {
      if (
        event.defaultPrevented ||
        event.isComposing ||
        event.repeat ||
        event.altKey ||
        event.shiftKey ||
        !(event.metaKey || event.ctrlKey) ||
        event.key.toLowerCase() !== "b"
      )
        return;
      // Rich-text editors own their formatting shortcuts.
      if (event.target instanceof HTMLElement && event.target.isContentEditable) return;
      event.preventDefault();
      setCollapsed(value => !value);
    };
    window.addEventListener("keydown", toggleNavigation);
    return () => window.removeEventListener("keydown", toggleNavigation);
  }, []);
  return (
    <div className="terminal">
      <aside
        className={`activity-rail ${collapsed ? "collapsed" : "expanded"}`}
        aria-keyshortcuts="Meta+B Control+B"
      >
        <div className="rail-brand" title={`Asterion Terminal · ${t("切换导航")} · ⌘B / Ctrl+B`}>
          <BrandMark size={20} />
        </div>
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
        <header className="workbench-header" hidden={!showToolbar}>
          <h1 className="sr-only">{title}</h1>
          <div className="workbench-context">{toolbar}</div>
        </header>
        {notices}
        {body}
        {dock}
        <footer className="status-bar">{status}</footer>
      </div>
    </div>
  );
}
