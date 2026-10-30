import type { CommandHandler, CommandCall } from "../bridge/commands";
import { useState } from "react";
import { readableCtpConnection } from "../bridge/client";
import type { CtpConnection, Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsDialog } from "./SettingsDialog";
import { SettingsSection } from "./Section";
const t = (key: string) => translate("host", key);
const blank = {
  name: "",
  broker_id: "",
  user_id: "",
  app_id: "",
  trade_front: "",
  market_front: "",
};
// The broker accounts the Terminal logs in to for market data and the contract
// catalog; exactly one of them supplies market data. Trading accounts belong
// to the trading hosts.
export function CtpConnections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const entries = snapshot?.ctp_connections ?? [];
  const connections = entries.filter(readableCtpConnection);
  const unreadable = entries.filter(entry => !readableCtpConnection(entry));
  const [editing, setEditing] = useState<CtpConnection | null>(null);
  const [draft, setDraft] = useState(blank);
  const [error, setError] = useState<DisplayError>("");
  const [removing, setRemoving] = useState("");
  // The form stays out of the way until an account is added or edited.
  const [adding, setAdding] = useState(false);
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
  const edit = (connection: CtpConnection | null) => {
    setEditing(connection);
    setError("");
    setRemoving("");
    setDraft(
      connection
        ? {
            name: connection.name,
            broker_id: connection.broker_id,
            user_id: connection.user_id,
            app_id: connection.app_id,
            trade_front: connection.trade_front,
            market_front: connection.market_front,
          }
        : blank,
    );
  };
  const field = (
    key: keyof typeof blank,
    label: string,
    options: { maxLength: number; placeholder?: string },
  ) => (
    <label>
      {label}
      <input
        aria-label={label}
        required
        maxLength={options.maxLength}
        placeholder={options.placeholder}
        value={draft[key]}
        onChange={event => setDraft({ ...draft, [key]: event.target.value.trim() })}
      />
    </label>
  );
  return (
    <SettingsSection
      title={t("账户")}
      label={t("CTP 行情账户")}
      description={t(
        "用于登录行情和查询合约目录，其中一个账户提供行情。交易账户在交易主机上配置，见交易连接。",
      )}
      actions={
        <button
          className="primary"
          disabled={busy}
          onClick={() => {
            edit(null);
            setAdding(true);
          }}
        >
          {t("添加账户")}
        </button>
      }
    >
      {unreadable.map(entry => (
        <section className="settings-card" key={entry.id} aria-label={entry.name}>
          <header>
            <h3>{entry.name}</h3>
          </header>
          <p role="alert">{t("连接文件无法读取，已保留原文件供检查")}</p>
        </section>
      ))}
      {connections.map(connection => (
        <section className="settings-card" key={connection.id} aria-label={connection.name}>
          <header>
            <h3>{connection.name}</h3>
            {connection.id === snapshot?.ctp_market && (
              <span className="settings-badge accent">{t("用于行情")}</span>
            )}
          </header>
          <dl>
            <dt>{t("经纪商代码")}</dt>
            <dd>{connection.broker_id}</dd>
            <dt>{t("投资者账号")}</dt>
            <dd>{connection.user_id}</dd>
            <dt>AppID</dt>
            <dd>{connection.app_id}</dd>
            <dt>{t("交易前置（合约目录）")}</dt>
            <dd>{connection.trade_front}</dd>
            <dt>{t("行情前置")}</dt>
            <dd>{connection.market_front}</dd>
          </dl>
          <footer>
            {connection.id !== snapshot?.ctp_market && (
              <button
                disabled={busy}
                onClick={() => void run("ctp.connections.market", { id: connection.id })}
              >
                {t("用于行情")}
              </button>
            )}
            <button
              disabled={busy}
              onClick={() => {
                edit(connection);
                setAdding(true);
              }}
            >
              {t("编辑账户")}
            </button>
            {removing === connection.id ? (
              <button
                className="danger"
                disabled={busy}
                onClick={async () => {
                  if (
                    await run("ctp.connections.remove", {
                      id: connection.id,
                      revision: connection.revision,
                    })
                  )
                    edit(null);
                }}
              >
                {t("确认删除账户")}
              </button>
            ) : (
              <button disabled={busy} onClick={() => setRemoving(connection.id)}>
                {t("删除账户")}
              </button>
            )}
          </footer>
        </section>
      ))}
      {!adding && !connections.length && !unreadable.length && (
        <p className="settings-hint">{t("还没有 CTP 账户。")}</p>
      )}
      {adding && (
        <SettingsDialog
          title={t(editing ? "编辑账户" : "新建账户")}
          busy={busy}
          onCancel={() => {
            edit(null);
            setAdding(false);
          }}
          onSubmit={async event => {
            event.preventDefault();
            if (
              await run("ctp.connections.save", {
                id: editing?.id ?? `ctp-${crypto.randomUUID()}`,
                revision: editing?.revision ?? "",
                ...draft,
                name: draft.name.trim(),
              })
            ) {
              edit(null);
              setAdding(false);
            }
          }}
        >
          <fieldset disabled={busy} className="futures-fields">
            <label>
              {t("账户名称")}
              <input
                aria-label={t("账户名称")}
                required
                maxLength={128}
                value={draft.name}
                onChange={event => setDraft({ ...draft, name: event.target.value })}
              />
            </label>
            {field("broker_id", t("经纪商代码"), { maxLength: 10 })}
            {field("user_id", t("投资者账号"), { maxLength: 15 })}
            {field("app_id", "AppID", { maxLength: 32 })}
            {field("trade_front", t("交易前置"), { maxLength: 64, placeholder: "tcp://host:port" })}
            {field("market_front", t("行情前置"), {
              maxLength: 64,
              placeholder: "tcp://host:port",
            })}
          </fieldset>
          {/* Next to the buttons it answers. */}
          {error && (
            <p className="alert" role="alert">
              <ErrorNotice error={error} />
            </p>
          )}
          <div className="source-actions">
            <button
              type="button"
              disabled={busy}
              onClick={() => {
                edit(null);
                setAdding(false);
              }}
            >
              {t("取消")}
            </button>
            <button className="primary" disabled={busy}>
              {t("保存账户")}
            </button>
          </div>
        </SettingsDialog>
      )}
      {!adding && error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </SettingsSection>
  );
}
