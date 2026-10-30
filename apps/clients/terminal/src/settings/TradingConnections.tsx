import { useState } from "react";
import type { CommandHandler } from "../bridge/commands";
import type { Snapshot, TradingConnection } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsDialog } from "./SettingsDialog";
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
  const bound = (id: string) => !!snapshot?.live[id];
  const edit = (entry: TradingConnection | null) => {
    setEditing(entry);
    setDraft(entry ? { ...entry, port: String(entry.port) } : blank);
    setAdding(true);
    setError("");
  };
  return (
    <section className="settings-cards" aria-label={t("交易服务连接")}>
      <h3>{t("交易服务连接")}</h3>
      <p>{t("每条连接对应交易主机上的一个账户实例。服务由交易主机管理。")}</p>
      <label>
        {t("本机客户端证书指纹")}
        <input readOnly value={snapshot?.trading_fingerprint ?? ""} />
      </label>
      {!snapshot?.trading_fingerprint && (
        <button
          disabled={busy}
          onClick={async () => {
            try {
              await trade("trading.identity.prepare", {});
              setError("");
            } catch (failure) {
              setError(asDisplayError(failure));
            }
          }}
        >
          {t("生成客户端证书")}
        </button>
      )}
      <p>{t("将本机指纹填入交易主机配置，再将主机指纹填入下面的连接。私钥保留在各自机器上。")}</p>
      {entries.map(entry => (
        <section className="settings-card" key={entry.id}>
          <h4>{entry.name}</h4>
          {"error" in entry ? (
            <p role="alert">{t("连接文件无法读取，已保留原文件供检查")}</p>
          ) : (
            <>
              <p>
                {entry.host}:{entry.port} · {entry.instance}
              </p>
              <code>{entry.fingerprint}</code>
              {snapshot?.live[entry.id]?.account && (
                <p>
                  {snapshot.live[entry.id].account.broker.broker_id} ·{" "}
                  {snapshot.live[entry.id].account.broker.user_id} ·{" "}
                  {snapshot.live[entry.id].account.broker.front}
                </p>
              )}
              <footer>
                <button
                  disabled={busy}
                  onClick={async () => {
                    try {
                      await trade(bound(entry.id) ? "live.close" : "live.open", {
                        account: entry.id,
                      });
                      setError("");
                    } catch (failure) {
                      setError(asDisplayError(failure));
                    }
                  }}
                >
                  {t(bound(entry.id) ? "断开交易服务连接" : "连接交易服务")}
                </button>
                <button disabled={busy || bound(entry.id)} onClick={() => edit(entry)}>
                  {t("编辑连接")}
                </button>
                <button
                  disabled={busy || bound(entry.id)}
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
              </footer>
            </>
          )}
        </section>
      ))}
      <button disabled={busy} onClick={() => edit(null)}>
        {t("添加交易连接")}
      </button>
      {adding && (
        <SettingsDialog
          title={t("交易服务连接")}
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
          <fieldset disabled={busy}>
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
                  disabled={key !== "name" && !!editing && bound(editing.id)}
                  type={key === "port" ? "number" : "text"}
                  min={key === "port" ? 1 : undefined}
                  max={key === "port" ? 65535 : undefined}
                  value={draft[key]}
                  onChange={event => setDraft({ ...draft, [key]: event.target.value })}
                />
              </label>
            ))}
          </fieldset>
          <ErrorNotice error={error} />
          <button type="button" onClick={() => setAdding(false)} disabled={busy}>
            {t("取消")}
          </button>
          <button disabled={busy}>{t("保存连接")}</button>
        </SettingsDialog>
      )}
      {!adding && <ErrorNotice error={error} />}
    </section>
  );
}
