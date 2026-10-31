import type { CommandHandler, CommandCall } from "../bridge/commands";
import { useState } from "react";
import type { DataCredential, Snapshot } from "../bridge/client";
import { translate, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsDialog } from "./SettingsDialog";
import { SettingsSection } from "./Section";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
// The lab's one source that takes a credential, and the bounds the Terminal
// enforces when it is saved.
const tushare = "asterion.data.tushare";
const rateMax = 500;
// The Tushare token the lab acquires with. A wrong one shows when an
// acquisition is refused.
export function DataSources({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const entries = snapshot?.data_credentials ?? [];
  const entry = entries.find(item => item.provider === tushare);
  const unreadable = !!entry && "error" in entry;
  const saved = entry && !("error" in entry) ? (entry as DataCredential) : undefined;
  const ready = !!saved?.credential_ready;
  const [editing, setEditing] = useState(false);
  const [credential, setCredential] = useState("");
  const [remember, setRemember] = useState(false);
  const [rate, setRate] = useState("60");
  const [error, setError] = useState<DisplayError>("");
  const [clearing, setClearing] = useState("");
  const run = async (...args: CommandCall) => {
    setError("");
    try {
      await trade(...args);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  const open = () => {
    setEditing(true);
    setCredential("");
    setError("");
    setClearing("");
    setRemember(saved?.remember ?? false);
    setRate(String(saved?.requests_per_minute ?? 60));
  };
  // Credentials saved for other providers before stay listed, so they can be cleared.
  const others = entries.filter(item => item.provider !== tushare);
  return (
    <SettingsSection
      title={t("历史数据供应商")}
      label={t("数据源")}
      description={t("采集历史数据要用的供应商凭据。同一账号的全部采集共享一个每分钟请求额度。")}
    >
      <section className="settings-card" aria-label="Tushare">
        <header>
          <h3>Tushare</h3>
          {unreadable ? (
            <span className="settings-badge warning">{t("设置文件无法读取")}</span>
          ) : ready ? (
            <span className="settings-badge accent">{t("已设置")}</span>
          ) : saved ? (
            <span className="settings-badge warning">{t("需要重新输入")}</span>
          ) : (
            <span className="settings-badge">{t("未设置")}</span>
          )}
        </header>
        <dl>
          <dt>{t("提供的数据")}</dt>
          <dd>{t("日线、交易日历、合约表、1 分钟线")}</dd>
          {saved && (
            <>
              <dt>Token</dt>
              <dd>
                {t(saved.remember ? "已保存到本机钥匙串" : "仅本次运行有效，退出后需重新输入")}
              </dd>
              <dt>{t("每分钟请求上限")}</dt>
              <dd>{saved.requests_per_minute}</dd>
            </>
          )}
        </dl>
        {unreadable && <p role="alert">{t("已保留原文件供检查；重新设置会覆盖它。")}</p>}
        <footer>
          <button className={ready ? undefined : "primary"} disabled={busy} onClick={open}>
            {t("设置 {label}", { label: "Token" })}
          </button>
          {(saved || unreadable) && (
            <ClearButton
              busy={busy}
              confirming={clearing === tushare}
              ask={() => setClearing(tushare)}
              clear={() => void run("data.credentials.clear", { provider: tushare })}
            />
          )}
        </footer>
      </section>
      {others.map(item => (
        <section className="settings-card" key={item.provider} aria-label={item.provider}>
          <header>
            <h3>{item.provider}</h3>
            <span className="settings-badge warning">{t("已不使用")}</span>
          </header>
          <footer>
            <ClearButton
              busy={busy}
              confirming={clearing === item.provider}
              ask={() => setClearing(item.provider)}
              clear={() => void run("data.credentials.clear", { provider: item.provider })}
            />
          </footer>
        </section>
      ))}
      {!editing && error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {editing && (
        <SettingsDialog
          title={t("设置 {label}", { label: "Token" })}
          busy={busy}
          onCancel={() => setEditing(false)}
          onSubmit={async event => {
            event.preventDefault();
            const done = await run("data.credentials.save", {
              provider: tushare,
              credential,
              remember,
              requests_per_minute: Number(rate),
            });
            setCredential("");
            if (done) setEditing(false);
          }}
        >
          <fieldset disabled={busy} className="futures-fields">
            <label>
              Token
              <input
                aria-label="Token"
                type="password"
                autoComplete="off"
                autoFocus
                maxLength={256}
                required={!ready}
                placeholder={ready ? t("留空则保留现有凭据") : undefined}
                value={credential}
                onChange={event => setCredential(event.target.value)}
              />
            </label>
            <label>
              {t("每分钟请求上限")}
              <input
                aria-label={t("每分钟请求上限")}
                type="number"
                required
                min={1}
                max={rateMax}
                value={rate}
                onChange={event => setRate(event.target.value)}
              />
            </label>
          </fieldset>
          <label className="native-plugin-choice">
            <input
              type="checkbox"
              checked={remember}
              onChange={event => setRemember(event.target.checked)}
            />
            {t("保存到本机钥匙串，下次启动不用再输入")}
          </label>
          {error && (
            <p className="alert" role="alert">
              <ErrorNotice error={error} />
            </p>
          )}
          <div className="source-actions">
            <button type="button" disabled={busy} onClick={() => setEditing(false)}>
              {t("取消")}
            </button>
            <button className="primary" disabled={busy}>
              {t("保存")}
            </button>
          </div>
        </SettingsDialog>
      )}
    </SettingsSection>
  );
}
function ClearButton({
  busy,
  confirming,
  ask,
  clear,
}: {
  busy: boolean;
  confirming: boolean;
  ask: () => void;
  clear: () => void;
}) {
  return (
    <button disabled={busy} onClick={confirming ? clear : ask}>
      {t(confirming ? "确认清除" : "清除")}
    </button>
  );
}
