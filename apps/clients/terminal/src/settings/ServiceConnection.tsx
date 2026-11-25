import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import { SettingsDialog } from "./SettingsDialog";
import { SettingsSection } from "./Section";
import { ServiceFacts } from "./ServiceFacts";
import { ActionError, useActionError } from "../host/action-error";
import { asDisplayError, type DisplayError } from "../i18n/errors";
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
  const namespace = `asterion.terminal.${service === "market" ? "futures-market" : "data-workbench"}`;
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
                type="button"
                className={clearing ? "danger" : "danger-quiet"}
                disabled={busy}
                onBlur={() => setClearing(false)}
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
            <button type="button" className="primary" disabled={busy} onClick={edit}>
              {t(endpoint ? word.edit : word.add)}
            </button>
          </>
        )
      }
    >
      {saved && "error" in saved && <p role="alert">{t(word.unreadable)}</p>}
      {endpoint && (
        <>
          <ServiceFacts
            key={`${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`}
            address={`${endpoint.host}:${endpoint.port}`}
            service={service}
          />
          <ActionError error={error} namespace={namespace} />
        </>
      )}
      {editing && (
        <SettingsDialog
          title={t(endpoint ? word.edit : word.add)}
          busy={busy}
          onCancel={() => setEditing(false)}
          onSubmit={async event => {
            event.preventDefault();
            try {
              await trade(`${service}.endpoint.save`, {
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
          <fieldset disabled={busy} className="futures-fields">
            <label>
              {t("主机地址")}
              <input
                aria-label={t("主机地址")}
                required
                maxLength={253}
                placeholder="192.168.50.20"
                spellCheck={false}
                autoCorrect="off"
                autoCapitalize="none"
                value={host}
                onChange={event => setHost(event.target.value)}
              />
            </label>
            <label>
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
            <label className="settings-field-wide">
              {t(word.fingerprint)}
              <input
                aria-label={t(word.fingerprint)}
                required
                maxLength={95}
                spellCheck={false}
                autoCorrect="off"
                autoCapitalize="none"
                value={fingerprint}
                onChange={event => setFingerprint(event.target.value)}
              />
            </label>
          </fieldset>
          <ActionError error={refused} namespace={namespace} />
          <div className="source-actions">
            <button type="button" onClick={() => setEditing(false)} disabled={busy}>
              {t("取消")}
            </button>
            <button type="submit" className="primary" disabled={busy}>
              {t("保存连接")}
            </button>
          </div>
        </SettingsDialog>
      )}
    </SettingsSection>
  );
}
