import { connectionRequests } from "@asterion/ui-connection-settings/public";
import { useRequestClient } from "@asterion/runtime-client/useRequestClient";
import { sourceRequests } from "@asterion/ui-data-panel/requests";
import { extensionRequests } from "@asterion/ui-extension-settings/requests";
import { renderSetting } from "@asterion/workbench/extensions/modules";
import { useEffect, useState } from "react";
import { useConnection } from "@asterion/desktop-bridge/useConnection";
import { nativeDesktop, stopDesktop } from "@asterion/desktop-bridge/desktop";
import { useWorkspaceAssembly } from "./assembly";
import { useSettingsCategory } from "@asterion/workbench/settings/category";
import { usePreferences } from "@asterion/ui-kit/preferences";
import { AccountEntry } from "@asterion/ui-account-settings/AccountEntry";
import { TerminalSecurity } from "@asterion/ui-account-settings/TerminalSecurity";
import { useDesktopAccount } from "@asterion/ui-account-settings/useDesktopAccount";
export function Settings() {
  const { composition: distribution } = useWorkspaceAssembly();
  const entries = [...distribution.settings.all()].sort(
    (a, b) => a.order - b.order,
  );
  const categories = entries.map((entry) => entry.title);
  const [category, setCategory] = useSettingsCategory(categories);
  const selected = entries.find((entry) => entry.title === category)!;
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const connection = useConnection(false);
  const sourcesApi = useRequestClient(
    connection.token,
    connection.connected,
    sourceRequests,
    "sources",
  );
  const extensionsApi = useRequestClient(connection.token, connection.connected, extensionRequests, "extensions");
  const connectionsApi=useRequestClient(connection.token,connection.connected,connectionRequests,"connections");
  const preferences = usePreferences();
  const { account, setAccount, restoring } = useDesktopAccount(
    connection.token,
  );
  useEffect(() => {
    document.title = "设置 · Asterion Terminal";
  }, []);
  async function toggleService() {
    setBusy(true);
    setError("");
    try {
      if (connection.connected) {
        await stopDesktop();
        connection.setToken("");
      } else {
        await connection.connect(true);
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  if (nativeDesktop && restoring)
    return <div className="startup-screen">正在恢复桌面会话…</div>;
  if (nativeDesktop && !account)
    return <AccountEntry token={connection.token} onEnter={setAccount} />;
  return (
    <TerminalSecurity token={connection.token} account={account}>
      <div className="settings-window">
        <aside className="settings-sidebar">
          <div className="settings-brand">
            ✧ <b>Asterion</b>
          </div>
          <nav aria-label="设置分类">
            {categories.map((v) => (
              <button
                key={v}
                className={v === category ? "active" : ""}
                onClick={() => setCategory(v)}
              >
                {v}
              </button>
            ))}
          </nav>
          <span className="settings-hint">设置独立保存</span>
        </aside>
        <main className="settings-content">
          <h1>{category}</h1>
          {error && (
            <div role="alert" className="alert">
              {error}
            </div>
          )}
          {renderSetting(selected, {
            empty: {},
            appearance: { preferences },
            data: { api: sourcesApi },
            connections: { api: connectionsApi },
            extensions: { api: extensionsApi },
            services: {
              connection: {
                starting: connection.starting,
                connected: connection.connected,
                directory: connection.directory,
                setToken: connection.setToken,
              },
              busy,
              toggleService,
            },
          })}
        </main>
      </div>
    </TerminalSecurity>
  );
}
