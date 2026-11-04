import type { CommandHandler } from "../bridge/commands";
import { BrandMark } from "../ui/BrandMark";
import { Icon, type IconName } from "../ui/Icon";
import { SettingsSection } from "./Section";
import { NotificationCenter } from "../host/notifications";
import { version } from "../../electron/package.json";
import { categoryKey, selectedPageKey, settingsPage, type SettingsPage } from "./window";
import { ErrorNotice, type DisplayError } from "../i18n/errors";
import { translate, useLocale, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useState, useEffect } from "react";
import { savePreferences, usePreferences } from "../ui/preferences";
import { TradingConnections } from "./TradingConnections";
import { MarketSource } from "./MarketSource";
import { LocalLogsButton } from "../host/components/LocalLogsButton";
import type { Snapshot } from "@asterion/desktop-bridge/client";
// The pages in sidebar order, with the title each shows.
const pages: [SettingsPage, string, IconName][] = [
  ["preferences", "偏好设置", "sliders"],
  ["trading", "交易连接", "trading"],
  ["sources", "数据源", "data"],
  ["about", "关于", "info"],
];
export function Settings({
  snapshot,
  error,
  busy,
  trade,
  initialPage = "preferences",
}: {
  initialPage?: SettingsPage;
  snapshot: Snapshot | null;
  error: DisplayError;
  busy: boolean;
  trade: CommandHandler;
}) {
  const [page, setPage] = useState<SettingsPage>(initialPage);
  const [unsaved, setUnsaved] = useState(false);
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
        {error && (
          <p className="alert" role="alert">
            <ErrorNotice error={error} namespace="host" />
          </p>
        )}
        {busy && <p role="status">{t("正在处理…")}</p>}
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
                      setUnsaved(false);
                    } catch {
                      setUnsaved(true);
                    }
                  }}
                >
                  <option value="zh-CN">简体中文</option>
                  <option value="en-US">English</option>
                </select>
                {unsaved && (
                  <span className="action-error" role="alert">
                    {t("保存失败，请检查本机存储权限。")}
                  </span>
                )}
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
        {page === "sources" && <MarketSource snapshot={snapshot} busy={busy} trade={trade} />}
        {page === "about" && (
          <SettingsSection title="Asterion Terminal">
            <dl className="settings-facts">
              <dt>{t("版本")}</dt>
              <dd>{version}</dd>
              <dt title={t("在 Market 和交易主机登记这个指纹")}>{t("本机证书指纹")}</dt>
              <dd>
                <code className="client-fingerprint">{snapshot?.client_fingerprint}</code>
              </dd>
              <dt>{t("本机日志")}</dt>
              <dd>
                <LocalLogsButton />
              </dd>
            </dl>
          </SettingsSection>
        )}
        <footer className="settings-footer">
          <NotificationCenter />
        </footer>
      </main>
    </section>
  );
}
