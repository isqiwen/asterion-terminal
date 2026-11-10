import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import { SettingsSection } from "./Section";
import { ServiceBuild } from "./ServiceBuild";
import { ActionError, useActionError } from "../host/action-error";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
const words = {
  market: {
    edit: "编辑行情连接",
    add: "添加行情连接",
    unreadable: "行情服务设置文件无法读取，已保留原文件供检查。",
    fingerprint: "行情服务证书指纹",
    port: 7451,
  },
  warehouse: {
    edit: "编辑数据连接",
    add: "添加数据连接",
    unreadable: "数据服务设置文件无法读取，已保留原文件供检查。",
    fingerprint: "数据服务证书指纹",
    port: 7450,
  },
};

export function ServiceConnection({
  service,
  snapshot,
  busy,
  trade,
}: {
  service: "market" | "warehouse";
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const word = words[service];
  const saved = service === "market" ? snapshot?.market_endpoint : snapshot?.warehouse_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const [editing, setEditing] = useState(false);
  const [host, setHost] = useState("");
  const [fingerprint, setFingerprint] = useState("");
  const [port, setPort] = useState(String(word.port));
  const [clearing, setClearing] = useState(false);
  // What the form's input was refused for, shown above its buttons.
  const [refused, setRefused] = useState<DisplayError>("");
  const { error, attempt } = useActionError();
  const run = (...args: CommandCall) => attempt(() => trade(...args));
  const edit = () => {
    setHost(endpoint?.host ?? "");
    setPort(String(endpoint?.port ?? word.port));
    setFingerprint(endpoint?.fingerprint ?? "");
    setClearing(false);
    setRefused("");
    setEditing(true);
  };
  return (
    <SettingsSection
      title={t("连接")}
      actions={
        !editing && (
          <>
            {endpoint && (
              <button
                className={clearing ? "danger" : undefined}
                disabled={busy}
                onClick={async () => {
                  if (!clearing) {
                    setClearing(true);
                    return;
                  }
                  if (await run(`${service}.endpoint.clear`, {})) setClearing(false);
                }}
              >
                {t(clearing ? "确认清除" : "清除")}
              </button>
            )}
            <button className="primary" disabled={busy} onClick={edit}>
              {t(endpoint ? word.edit : word.add)}
            </button>
          </>
        )
      }
    >
      {saved && "error" in saved && <p role="alert">{t(word.unreadable)}</p>}
      {!editing && endpoint && (
        <>
          <p className="settings-hint">
            {t("地址")} <code>{`${endpoint.host}:${endpoint.port}`}</code>
          </p>
          <ServiceBuild
            key={`${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`}
            service={service}
          />
          <ActionError error={error} />
        </>
      )}
      {editing && (
        <form
          onSubmit={async event => {
            event.preventDefault();
            try {
              await trade(`${service}.endpoint.save`, {
                host: host.trim(),
                port: Number(port),
                fingerprint: fingerprint.trim(),
              });
              setEditing(false);
              // Saving drops the old live connection; the new one opens as at startup.
              if (service === "market") void run("market.open", {});
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
                {t(word.fingerprint)}
                <input
                  aria-label={t(word.fingerprint)}
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
              <button className="primary">{t("保存连接")}</button>
            </div>
          </fieldset>
        </form>
      )}
    </SettingsSection>
  );
}
