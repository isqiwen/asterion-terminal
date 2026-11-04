import { useState } from "react";
import type { CommandHandler } from "../bridge/commands";
import type { Snapshot, TradingConnection } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsDialog } from "./SettingsDialog";
import { SettingsSection } from "./Section";
import { ClientFingerprint } from "./ClientFingerprint";
const t = (key: string) => translate("host", key);
const blank = { name: "", host: "", port: "7460", instance: "", fingerprint: "" };

export function TradingConnections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const [editing, setEditing] = useState<TradingConnection | null>(null);
  const [adding, setAdding] = useState(false);
  const [draft, setDraft] = useState(blank);
  const [error, setError] = useState<DisplayError>("");
  const [removing, setRemoving] = useState("");
  const entries = snapshot?.trading_connections ?? [];
  const edit = (entry: TradingConnection | null) => {
    setEditing(entry);
    setDraft(entry ? { ...entry, port: String(entry.port) } : blank);
    setAdding(true);
    setError("");
  };
  return (
    <>
      <SettingsSection
        title={t("交易主机")}
        actions={
          <button disabled={busy} onClick={() => edit(null)}>
            {t("添加交易连接")}
          </button>
        }
      >
        <ClientFingerprint value={snapshot?.client_fingerprint} />
        {!entries.length && <p className="settings-hint">{t("还没有交易连接。")}</p>}
        <div className="settings-list">
          {entries.map(entry => (
            <section className="settings-item" key={entry.id} aria-label={entry.name}>
              <div className="settings-item-row">
                <div>
                  <h3>{entry.name}</h3>
                  {"error" in entry ? (
                    <p role="alert">{t("连接文件无法读取，已保留原文件供检查")}</p>
                  ) : (
                    <p>
                      {entry.host}:{entry.port} · {entry.instance}
                    </p>
                  )}
                </div>
                {!("error" in entry) && (
                  <div className="settings-actions">
                    <button disabled={busy} onClick={() => edit(entry)}>
                      {t("编辑连接")}
                    </button>
                    <button
                      className={removing === entry.id ? "danger" : undefined}
                      disabled={busy}
                      onClick={async () => {
                        if (removing !== entry.id) {
                          setRemoving(entry.id);
                          return;
                        }
                        try {
                          await trade("trading.connections.remove", {
                            id: entry.id,
                            revision: entry.revision,
                          });
                          setRemoving("");
                          setError("");
                        } catch (failure) {
                          setError(asDisplayError(failure));
                        }
                      }}
                    >
                      {t(removing === entry.id ? "确认删除连接" : "删除连接")}
                    </button>
                  </div>
                )}
              </div>
              {!("error" in entry) && (
                <details>
                  <summary>{t("技术详情")}</summary>
                  <dl>
                    <dt>{t("服务端证书指纹")}</dt>
                    <dd>
                      <code>{entry.fingerprint}</code>
                    </dd>
                  </dl>
                </details>
              )}
            </section>
          ))}
        </div>
        {!adding && error && (
          <p className="alert" role="alert">
            <ErrorNotice error={error} />
          </p>
        )}
        {adding && (
          <SettingsDialog
            title={t(editing ? "编辑连接" : "添加交易连接")}
            busy={busy}
            onCancel={() => setAdding(false)}
            onSubmit={async event => {
              event.preventDefault();
              try {
                await trade("trading.connections.save", {
                  id: editing?.id ?? `trading-${crypto.randomUUID()}`,
                  revision: editing?.revision ?? "",
                  name: draft.name.trim(),
                  host: draft.host.trim(),
                  port: Number(draft.port),
                  instance: draft.instance.trim(),
                  fingerprint: draft.fingerprint.trim(),
                });
                setAdding(false);
                setError("");
              } catch (failure) {
                setError(asDisplayError(failure));
              }
            }}
          >
            <fieldset disabled={busy} className="futures-fields">
              {(
                [
                  ["name", "连接名称"],
                  ["host", "交易主机地址"],
                  ["port", "端口"],
                  ["instance", "交易主机账户实例名"],
                  ["fingerprint", "服务端证书指纹"],
                ] as const
              ).map(([key, label]) => (
                <label key={key}>
                  {t(label)}
                  <input
                    required
                    type={key === "port" ? "number" : "text"}
                    min={key === "port" ? 1 : undefined}
                    max={key === "port" ? 65535 : undefined}
                    value={draft[key]}
                    onChange={event => setDraft({ ...draft, [key]: event.target.value })}
                  />
                </label>
              ))}
            </fieldset>
            {error && (
              <p className="alert" role="alert">
                <ErrorNotice error={error} />
              </p>
            )}
            <div className="source-actions">
              <button type="button" onClick={() => setAdding(false)} disabled={busy}>
                {t("取消")}
              </button>
              <button className="primary" disabled={busy}>
                {t("保存连接")}
              </button>
            </div>
          </SettingsDialog>
        )}
      </SettingsSection>
    </>
  );
}
