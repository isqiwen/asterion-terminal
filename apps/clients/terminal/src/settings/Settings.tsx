import type { CommandHandler } from "../bridge/commands";
import { BrandMark } from "../ui/BrandMark";
import { Icon, type IconName } from "../ui/Icon";
import { version } from "../../electron/package.json";
import { categoryKey, selectedPageKey, settingsPage, type SettingsPage } from "./window";
import { ErrorNotice, type DisplayError } from "../i18n/errors";
import { translate, useLocale, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useState, useEffect } from "react";
import { savePreferences, usePreferences } from "../ui/preferences";
import { DataSources } from "./DataSources";
import { TradingConnections } from "./TradingConnections";
import { CtpConnections } from "./CtpConnections";
import { NativePlugins } from "./NativePlugins";
import { ResourceCatalogue } from "./ResourceCatalogue";
import { DataBackup } from "./DataBackup";
import { Connections } from "./Connections";
import type { Snapshot } from "@asterion/desktop-bridge/client";
export function Settings({
  snapshot,
  refresh,
  error,
  busy,
  trade,
  initialPage = "preferences",
}: {
  initialPage?: SettingsPage;
  snapshot: Snapshot | null;
  refresh: () => void;
  error: DisplayError;
  busy: boolean;
  trade: CommandHandler;
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
          <BrandMark size={24} />
          <span>ASTERION</span>
        </div>
        <nav>
          {[
            ["preferences", t("偏好设置"), "settings"],
            ["connections", t("本机服务"), "connections"],
            ["ctp", t("CTP 账户"), "connections"],
            ["sources", t("数据源"), "connections"],
            ["plugins", t("插件"), "plugins"],
            ["resources", t("业务资产目录"), "settings"],
            ["backup", t("数据备份与恢复"), "settings"],
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
              connections: t("本机服务"),
              ctp: t("CTP 账户"),
              sources: t("数据源"),
              plugins: t("插件"),
              backup: t("数据备份与恢复"),
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
        {page === "resources" && <ResourceCatalogue snapshot={snapshot} />}
        {page === "backup" && <DataBackup snapshot={snapshot} />}
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
        {page === "ctp" && (
          <>
            <TradingConnections snapshot={snapshot} busy={busy} trade={trade} />
            <CtpConnections snapshot={snapshot} busy={busy} trade={trade} />
          </>
        )}
        {page === "sources" && <DataSources snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "connections" && <Connections snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "plugins" && <NativePlugins snapshot={snapshot} busy={busy} trade={trade} />}
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
