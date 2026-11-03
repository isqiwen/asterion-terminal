import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import { SettingsSection } from "./Section";
import { notify } from "../host/notifications";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
export function MarketSource({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const saved = snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const [editing, setEditing] = useState(false);
  const [host, setHost] = useState("");
  const [fingerprint, setFingerprint] = useState("");
  const [port, setPort] = useState("7450");
  const [clearing, setClearing] = useState(false);
  // What the form's input was refused for, shown above its buttons.
  const [refused, setRefused] = useState<DisplayError>("");
  const run = async (...args: CommandCall) => {
    try {
      await trade(...args);
      return true;
    } catch (reason) {
      notify(reason);
      return false;
    }
  };
  const edit = () => {
    setHost(endpoint?.host ?? "");
    setPort(String(endpoint?.port ?? 7450));
    setFingerprint(endpoint?.fingerprint ?? "");
    setClearing(false);
    setRefused("");
    setEditing(true);
  };
  return (
    <SettingsSection
      title={t("远程行情服务")}
      description={t(
        "Market 独立接入与录制行情，向所有客户端提供实时流和历史数据。通过证书验证服务与客户端身份，柜台和历史供应商凭据在 Market 主机配置。",
      )}
      actions={
        !editing && (
          <button className={endpoint ? undefined : "primary"} disabled={busy} onClick={edit}>
            {t(endpoint ? "修改地址" : "填写地址")}
          </button>
        )
      }
    >
      <p className="settings-hint">
        {t("在 Market 主机登记本机证书指纹，并授予只读或采集管理权限。")}
      </p>
      {snapshot?.trading_fingerprint ? (
        <code>{snapshot.trading_fingerprint}</code>
      ) : (
        <button disabled={busy} onClick={() => run("trading.identity.prepare", {})}>
          {t("生成本机证书")}
        </button>
      )}
      {saved && "error" in saved && (
        <p role="alert">{t("行情服务设置文件无法读取，已保留原文件供检查。")}</p>
      )}
      {!editing &&
        (endpoint ? (
          <div className="settings-item-row">
            <p className="settings-hint">
              {t("地址")} <code>{`${endpoint.host}:${endpoint.port}`}</code>
            </p>
            <div className="settings-actions">
              <button
                className={clearing ? "danger" : undefined}
                disabled={busy}
                onClick={async () => {
                  if (!clearing) {
                    setClearing(true);
                    return;
                  }
                  if (await run("market.endpoint.clear", {})) setClearing(false);
                }}
              >
                {t(clearing ? "确认清除" : "清除")}
              </button>
            </div>
          </div>
        ) : (
          !(saved && "error" in saved) && (
            <p className="settings-hint">{t("尚未填写行情服务地址。")}</p>
          )
        ))}
      {editing && (
        <form
          onSubmit={async event => {
            event.preventDefault();
            try {
              await trade("market.endpoint.save", {
                host: host.trim(),
                port: Number(port),
                fingerprint: fingerprint.trim(),
              });
              setEditing(false);
            } catch (reason) {
              setRefused(asDisplayError(reason));
            }
          }}
        >
          <fieldset disabled={busy}>
            <div className="settings-fields">
              <label className="settings-field">
                {t("主机地址")}
                <input
                  aria-label={t("主机地址")}
                  required
                  maxLength={253}
                  placeholder="192.168.1.20"
                  value={host}
                  onChange={event => setHost(event.target.value)}
                />
              </label>
              <label className="settings-field">
                {t("端口")}
                <input
                  aria-label={t("端口")}
                  type="number"
                  required
                  min={1}
                  max={65535}
                  value={port}
                  onChange={event => setPort(event.target.value)}
                />
              </label>
              <label className="settings-field">
                {t("Market 服务证书指纹")}
                <input
                  aria-label={t("Market 服务证书指纹")}
                  required
                  maxLength={95}
                  value={fingerprint}
                  onChange={event => setFingerprint(event.target.value)}
                />
              </label>
            </div>
            {refused && (
              <p className="settings-error" role="alert">
                <ErrorNotice error={refused} />
              </p>
            )}
            <div className="settings-actions">
              <button type="button" onClick={() => setEditing(false)}>
                {t("取消")}
              </button>
              <button className="primary">{t("保存地址")}</button>
            </div>
          </fieldset>
        </form>
      )}
    </SettingsSection>
  );
}
