import { useState } from "react";
import type {
  DataCredential,
  NativeHistorySource,
  Snapshot,
  TerminalCommand,
} from "../bridge/client";
import { translate, useLocale, type MessageValues } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsDialog } from "./SettingsDialog";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
const checkStates = [
  "接口访问成功",
  "尚未验证",
  "凭据无效",
  "接口权限不足",
  "请求受到限流",
  "网络连接失败",
  "验证失败",
];
const dataType = (source: NativeHistorySource) =>
  source.intervals.length === 1 && source.intervals[0] === 0 ? "日 K 线" : "分钟 K 线";
// One card per data provider. A provider takes a single credential, used by
// all of its data; saving it checks it at once, so the card says whether it
// works.
export function DataSources({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const { locale } = useLocale();
  const online = !!snapshot?.data?.online;
  const sources = snapshot?.data?.sources.filter(source => source.connection) ?? [];
  const entries = snapshot?.data_credentials ?? [];
  const providers = [...new Set(sources.map(source => source.plugin_id))];
  const [editing, setEditing] = useState("");
  const [credential, setCredential] = useState("");
  const [remember, setRemember] = useState(false);
  const [rate, setRate] = useState("60");
  const [error, setError] = useState<DisplayError>("");
  const [clearing, setClearing] = useState("");
  const run = async (method: TerminalCommand, params?: Record<string, unknown>) => {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  const describe = (provider: string) => {
    const own = sources.filter(source => source.plugin_id === provider);
    const schemas = own.map(source => source.connection!);
    const entry = entries.find(item => item.provider === provider);
    return {
      own,
      name: [...new Set(own.map(source => source.name))].join(" / ") || provider,
      label:
        (locale === "zh-CN" ? schemas[0]?.credential_label_zh : schemas[0]?.credential_label_en) ??
        t("凭据"),
      // One credential serves every source, so it has to fit the strictest.
      maxLength: Math.min(256, ...schemas.map(schema => schema.credential_max_length)),
      rateMax: Math.min(500, ...schemas.map(schema => schema.requests_per_minute_max)),
      rateDefault: Math.min(60, ...schemas.map(schema => schema.requests_per_minute_default)),
      rememberAllowed: schemas.every(schema => schema.remember_allowed),
      required: schemas.some(schema => schema.credential_required),
      unreadable: !!entry && "error" in entry,
      saved: entry && !("error" in entry) ? (entry as DataCredential) : undefined,
    };
  };
  const open = (provider: string) => {
    const { saved, rateDefault } = describe(provider);
    setEditing(provider);
    setCredential("");
    setError("");
    setClearing("");
    setRemember(saved?.remember ?? false);
    setRate(String(saved?.requests_per_minute ?? rateDefault));
  };
  // Saved entries whose plugin is not loaded still show, so they can be cleared.
  const orphans = online ? entries.filter(entry => !providers.includes(entry.provider)) : [];
  const current = editing ? describe(editing) : null;
  return (
    <section className="settings-cards" aria-label={t("数据源")}>
      {!online && (
        <div className="settings-card">
          <p role="status">{t("数据服务未连接，无法读取数据源。")}</p>
          <footer>
            <button disabled={busy} onClick={() => void run("node.data_tasks.local.open")}>
              {t("重新连接")}
            </button>
          </footer>
        </div>
      )}
      {online && !providers.length && !orphans.length && <p>{t("没有需要凭据的数据源。")}</p>}
      {providers.map(provider => {
        const { own, name, label, unreadable, saved } = describe(provider);
        const ready = !!saved?.credential_ready;
        const verification =
          snapshot?.credential_verification?.provider === provider
            ? snapshot.credential_verification
            : null;
        return (
          <section className="settings-card" key={provider} aria-label={name}>
            <header>
              <h3>{name}</h3>
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
              <dd>{[...new Set(own.map(source => t(dataType(source))))].join(" · ")}</dd>
              {saved && (
                <>
                  <dt>{label}</dt>
                  <dd>
                    {t(saved.remember ? "已保存到本机钥匙串" : "仅本次运行有效，退出后需重新输入")}
                  </dd>
                  <dt>{t("每分钟请求上限")}</dt>
                  <dd>{saved.requests_per_minute}</dd>
                </>
              )}
              {verification?.checks.map(check => {
                const source = own.find(item => item.id === check.source);
                return (
                  <VerificationRow
                    key={`${check.source}:${check.scope}`}
                    label={`${source ? t(dataType(source)) : check.source} · ${t(
                      check.scope === "catalog" ? "合约目录" : "历史数据接口",
                    )}`}
                    state={check.state}
                  />
                );
              })}
            </dl>
            <p className="subtle">
              {t("同一账号的下载共享当前数据服务的额度；多节点请分别分配份额。")}
            </p>
            {unreadable && <p role="alert">{t("已保留原文件供检查；重新设置会覆盖它。")}</p>}
            <footer>
              <button
                className={ready ? undefined : "primary"}
                disabled={busy}
                onClick={() => open(provider)}
              >
                {t("设置 {label}", { label })}
              </button>
              {ready && (
                <button
                  disabled={busy}
                  onClick={() => void run("data.credentials.verify", { provider })}
                >
                  {t("验证")}
                </button>
              )}
              {saved && ready && (
                <button
                  disabled={busy}
                  onClick={() =>
                    void run("data.download.budget.configure", {
                      source: own[0]!.id,
                      token: "",
                      requests_per_minute: saved.requests_per_minute,
                    })
                  }
                >
                  {t("应用共享额度")}
                </button>
              )}
              {(saved || unreadable) && (
                <ClearButton
                  busy={busy}
                  confirming={clearing === provider}
                  ask={() => setClearing(provider)}
                  clear={() => void run("data.credentials.clear", { provider })}
                />
              )}
            </footer>
          </section>
        );
      })}
      {orphans.map(entry => (
        <section className="settings-card" key={entry.provider} aria-label={entry.provider}>
          <header>
            <h3>{entry.provider}</h3>
            <span className="settings-badge warning">{t("数据源插件未启用")}</span>
          </header>
          <footer>
            <ClearButton
              busy={busy}
              confirming={clearing === entry.provider}
              ask={() => setClearing(entry.provider)}
              clear={() => void run("data.credentials.clear", { provider: entry.provider })}
            />
          </footer>
        </section>
      ))}
      {!editing && error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {current && (
        <SettingsDialog
          title={t("设置 {label}", { label: current.label })}
          busy={busy}
          onCancel={() => setEditing("")}
          onSubmit={async event => {
            event.preventDefault();
            const provider = editing;
            const saved = await run("data.credentials.save", {
              provider,
              credential,
              remember,
              requests_per_minute: Number(rate),
            });
            setCredential("");
            if (!saved) return;
            setEditing("");
            await run("data.credentials.verify", { provider });
          }}
        >
          <fieldset disabled={busy} className="futures-fields">
            <label>
              {current.label}
              <input
                aria-label={current.label}
                type="password"
                autoComplete="off"
                autoFocus
                maxLength={current.maxLength}
                required={current.required && !current.saved?.credential_ready}
                placeholder={current.saved?.credential_ready ? t("留空则保留现有凭据") : undefined}
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
                max={current.rateMax}
                value={rate}
                onChange={event => setRate(event.target.value)}
              />
            </label>
          </fieldset>
          {current.rememberAllowed && (
            <label className="native-plugin-choice">
              <input
                type="checkbox"
                checked={remember}
                onChange={event => setRemember(event.target.checked)}
              />
              {t("保存到本机钥匙串，下次启动不用再输入")}
            </label>
          )}
          {error && (
            <p className="alert" role="alert">
              <ErrorNotice error={error} />
            </p>
          )}
          <div className="source-actions">
            <button type="button" disabled={busy} onClick={() => setEditing("")}>
              {t("取消")}
            </button>
            <button className="primary" disabled={busy}>
              {t("保存并验证")}
            </button>
          </div>
        </SettingsDialog>
      )}
    </section>
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
function VerificationRow({ label, state }: { label: string; state: number }) {
  return (
    <>
      <dt>{label}</dt>
      <dd className={state === 0 ? "ok" : "bad"}>{t(checkStates[state] ?? "验证失败")}</dd>
    </>
  );
}
