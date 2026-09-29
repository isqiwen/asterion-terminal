import { Icon, type IconName } from "../ui/Icon";
import { version } from "../../electron/package.json";
import { categoryKey, selectedPageKey, settingsPage, type SettingsPage } from "./window";
import { ErrorNotice, type DisplayError } from "../i18n/errors";
import { translate, useLocale, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import type { TerminalPlugin } from "../../plugins/contract";
import { useState, useEffect } from "react";
import { savePreferences, usePreferences } from "../ui/preferences";
import { Connections } from "./Connections";
import type { TerminalCommand, Snapshot } from "@asterion/desktop-bridge/client";
export function Settings({
  plugins,
  snapshot,
  refresh,
  error,
  busy,
  trade,
  initialPage = "preferences",
}: {
  plugins: readonly TerminalPlugin[];
  initialPage?: SettingsPage;
  snapshot: Snapshot | null;
  refresh: () => void;
  error: DisplayError;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const [page, setPage] = useState<string>(initialPage);
  useEffect(() => {
    const select = (event: StorageEvent) => {
      if (event.key === categoryKey && event.newValue) {
        try {
          setPage(settingsPage(JSON.parse(event.newValue).page));
        } catch {
          /* Invalid UI selection does not change the current page. */
        }
      }
    };
    window.addEventListener("storage", select);
    return () => window.removeEventListener("storage", select);
  }, []);
  useEffect(() => {
    localStorage.setItem(selectedPageKey, page);
  }, [page]);
  const preferences = usePreferences();
  const [languageError, setLanguageError] = useState<DisplayError>("");
  const { locale, setLocale } = useLocale();
  return (
    <section className="settings-window" aria-label={t("终端设置")}>
      <aside className="settings-sidebar">
        <div className="settings-brand">
          <Icon name="asterion" size={24} />
          <span>ASTERION</span>
        </div>
        <nav>
          {[
            ["preferences", t("偏好设置"), "settings"],
            ["connections", t("连接与部署"), "connections"],
            ["plugins", t("插件"), "plugins"],
            ["about", t("关于"), "info"],
          ].map(([item, label, icon]) => (
            <button
              key={item}
              className={page === item ? "active" : ""}
              aria-current={page === item ? "page" : undefined}
              onClick={() => setPage(item)}
            >
              <Icon name={icon as IconName} size={16} />
              <span>{label}</span>
            </button>
          ))}
        </nav>
      </aside>
      <main className="settings-content">
        <h1>
          {
            {
              preferences: t("偏好设置"),
              connections: t("连接与部署"),
              plugins: t("插件"),
              about: t("关于"),
            }[page]
          }
        </h1>
        {languageError && (
          <p className="alert" role="alert">
            <ErrorNotice error={languageError} />
          </p>
        )}
        {error && (
          <p className="alert" role="alert">
            <ErrorNotice error={error} namespace="host" />
          </p>
        )}
        {busy && <p role="status">{t("正在处理…")}</p>}
        {page === "preferences" && (
          <>
            <label className="terminal-setting">
              {t("语言")}
              <select
                aria-label={t("语言")}
                value={locale}
                onChange={e => {
                  try {
                    setLocale(e.target.value as "zh-CN" | "en-US");
                    setLanguageError("");
                  } catch {
                    setLanguageError(t("保存失败，请检查本机存储权限。"));
                  }
                }}
              >
                <option value="zh-CN">简体中文</option>
                <option value="en-US">English</option>
              </select>
            </label>
            <label className="terminal-setting">
              {t("显示密度")}
              <select
                aria-label={t("显示密度")}
                value={preferences.density}
                onChange={e =>
                  savePreferences({
                    ...preferences,
                    density: e.target.value as "compact" | "comfortable",
                  })
                }
              >
                <option value="compact">{t("紧凑")}</option>
                <option value="comfortable">{t("舒适")}</option>
              </select>
            </label>
            <label className="terminal-setting">
              {t("涨跌配色")}
              <select
                aria-label={t("涨跌配色")}
                value={preferences.colors}
                onChange={e =>
                  savePreferences({
                    ...preferences,
                    colors: e.target.value as "china" | "international",
                  })
                }
              >
                <option value="china">{t("红涨绿跌")}</option>
                <option value="international">{t("绿涨红跌")}</option>
              </select>
            </label>
          </>
        )}
        {page === "connections" && <Connections snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "plugins" && (
          <>
            <p>{t("当前发行版内置能力")}</p>
            <table className="data-table">
              <thead>
                <tr>
                  <th>{t("插件")}</th>
                  <th>{t("类型")}</th>
                  <th>{t("状态")}</th>
                </tr>
              </thead>
              <tbody>
                {snapshot?.plugins.map(plugin => (
                  <tr key={plugin.id}>
                    <td>{plugin.id}</td>
                    <td>
                      {{
                        data: t("数据"),
                        execution: t("执行"),
                        storage: t("存储"),
                        tool: t("工具"),
                      }[plugin.kind] ?? plugin.kind}
                    </td>
                    <td>{t("可用")}</td>
                  </tr>
                ))}
                {plugins.map(plugin => (
                  <tr key={plugin.id}>
                    <td>{plugin.workspace.title}</td>
                    <td>{t("用户界面")}</td>
                    <td>{t("已注册 · 按需加载")}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <p>{t("当前为内置插件，动态安装、卸载与语言 SDK 尚未开放。")}</p>
          </>
        )}
        {page === "about" && (
          <>
            <h2>Asterion Terminal</h2>
            <p>
              {t("版本")} {version}
            </p>
            <p>{t("当前通过安装包更新")}</p>
            <details>
              <summary>{t("诊断信息")}</summary>
              <dl>
                <dt>{t("核心")}</dt>
                <dd>{error ? t("连接失败") : (snapshot?.core ?? t("未连接"))}</dd>
                <dt>{t("桌面外壳")}</dt>
                <dd>Electron · React / TypeScript</dd>
              </dl>
              <button disabled={busy} onClick={refresh}>
                {t("重新检测")}
              </button>
            </details>
          </>
        )}
      </main>
    </section>
  );
}
