import type { CommandHandler } from "../bridge/commands";
import { BrandMark } from "../ui/BrandMark";
import { Icon, type IconName } from "../ui/Icon";
import { SettingsSection } from "./Section";
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
import { RecorderSource } from "./RecorderSource";
import { StrategyRepository } from "./StrategyRepository";
import { NativePlugins } from "./NativePlugins";
import { ResourceCatalogue } from "./ResourceCatalogue";
import { DataBackup } from "./DataBackup";
import { Connections } from "./Connections";
import type { Snapshot } from "@asterion/desktop-bridge/client";
// The pages in sidebar order, with the title each shows.
const pages: [SettingsPage, string, IconName][] = [
  ["preferences", "偏好设置", "sliders"],
  ["connections", "本机服务", "connections"],
  ["trading", "交易连接", "trading"],
  ["sources", "数据源", "data"],
  ["research", "研究", "backtest"],
  ["plugins", "插件", "plugins"],
  ["resources", "业务资产目录", "catalog"],
  ["backup", "数据备份与恢复", "archive"],
  ["about", "关于", "info"],
];
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
  const [page, setPage] = useState<SettingsPage>(initialPage);
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
          {pages.map(([item, label, icon]) => (
            <button
              key={item}
              className={page === item ? "active" : ""}
              aria-current={page === item ? "page" : undefined}
              onClick={() => setPage(item)}
            >
              <Icon name={icon} size={16} />
              <span>{t(label)}</span>
            </button>
          ))}
        </nav>
      </aside>
      <main className="settings-content">
        <h1>{t(pages.find(([item]) => item === page)?.[1] ?? "偏好设置")}</h1>
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
          <SettingsSection title={t("显示")}>
            <div className="settings-fields">
              <label className="settings-field">
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
              <label className="settings-field">
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
              <label className="settings-field">
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
            </div>
          </SettingsSection>
        )}
        {page === "trading" && <TradingConnections snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "sources" && (
          <>
            <CtpConnections snapshot={snapshot} busy={busy} trade={trade} />
            <RecorderSource snapshot={snapshot} busy={busy} trade={trade} />
            <DataSources snapshot={snapshot} busy={busy} trade={trade} />
          </>
        )}
        {page === "research" && (
          <StrategyRepository snapshot={snapshot} busy={busy} trade={trade} />
        )}
        {page === "connections" && <Connections snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "plugins" && (
          <NativePlugins
            snapshot={snapshot}
            busy={busy}
            trade={trade}
            openServices={() => setPage("connections")}
          />
        )}
        {page === "about" && (
          <SettingsSection title="Asterion Terminal" description={`${t("版本")} ${version}`}>
            <p className="settings-hint">{t("新版本通过安装包更新。")}</p>
            <details>
              <summary>{t("诊断信息")}</summary>
              <dl>
                <dt>{t("核心")}</dt>
                <dd>{error ? t("连接失败") : (snapshot?.core ?? t("未连接"))}</dd>
                <dt>{t("桌面外壳")}</dt>
                <dd>Electron · React / TypeScript</dd>
              </dl>
            </details>
            <div className="settings-actions">
              <button disabled={busy} onClick={refresh}>
                {t("重新检测")}
              </button>
            </div>
          </SettingsSection>
        )}
      </main>
    </section>
  );
}
