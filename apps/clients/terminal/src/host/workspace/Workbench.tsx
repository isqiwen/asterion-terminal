import { translate, type MessageValues } from "../../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useState, useEffect, type ReactNode } from "react";
import { Icon } from "../../ui/Icon";
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
  showToolbar = true,
  navigation,
  rail,
  toolbar,
  notices,
  body,
  status,
}: {
  showToolbar?: boolean;
  navigation: readonly Navigation[];
  rail: ReactNode;
  toolbar: ReactNode;
  notices: ReactNode;
  body: ReactNode;
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
      <div className="terminal-main">
        <aside
          className={`activity-rail ${collapsed ? "collapsed" : "expanded"}`}
          aria-keyshortcuts="Meta+B Control+B"
        >
          <nav aria-label={t("业务工作区")} className="workspace-tabs">
            {navigation.map(item => (
              <button
                key={item.id}
                type="button"
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
          <div className="rail-actions">
            <button
              type="button"
              aria-expanded={!collapsed}
              aria-label={t(collapsed ? "展开导航栏" : "收起导航栏")}
              title={`${t(collapsed ? "展开导航栏" : "收起导航栏")} · ⌘B / Ctrl+B`}
              onClick={() => setCollapsed(value => !value)}
            >
              <Icon name={collapsed ? "chevronRight" : "chevronLeft"} size={16} />
            </button>
            {rail}
          </div>
        </aside>
        <div className="terminal-content">
          <header className="workbench-header" hidden={!showToolbar}>
            <div className="workbench-context">{toolbar}</div>
          </header>
          {notices}
          {body}
        </div>
      </div>
      <footer className="status-bar">{status}</footer>
    </div>
  );
}
