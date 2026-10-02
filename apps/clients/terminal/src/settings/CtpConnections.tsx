import { useEffect, useRef, useState, type ReactNode } from "react";
import { readableCtpConnection } from "../bridge/client";
import type { CtpConnection, Snapshot, TerminalCommand } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);
const blank = {
  name: "",
  broker_id: "",
  user_id: "",
  app_id: "",
  trade_front: "",
  market_front: "",
};
// The account form opens over the list: adding or editing never moves the
// accounts being looked at, and Escape or Cancel leaves them untouched.
function AccountDialog({
  title,
  busy,
  onCancel,
  onSubmit,
  children,
}: {
  title: string;
  busy: boolean;
  onCancel: () => void;
  onSubmit: (event: React.FormEvent<HTMLFormElement>) => void;
  children: ReactNode;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const value = dialog.current!;
    value.showModal();
    return () => value.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="service-action-dialog ctp-account-dialog"
      aria-labelledby="ctp-account-title"
      onCancel={event => {
        event.preventDefault();
        if (!busy) onCancel();
      }}
    >
      <h2 id="ctp-account-title">{title}</h2>
      <form onSubmit={onSubmit}>{children}</form>
    </dialog>
  );
}
// One place for the counter accounts the broker issues. Each can trade on the
// Trading page; exactly one of them supplies market data.
export function CtpConnections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
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
  // Counter details of an account that already trades are fixed.
  const fixed = !!editing?.trading_record;
  const field = (
    key: keyof typeof blank,
    label: string,
    options: { maxLength: number; placeholder?: string },
  ) => (
    <label>
      {label}
      <input
        aria-label={label}
        disabled={fixed && key !== "market_front"}
        required
        maxLength={options.maxLength}
        placeholder={options.placeholder}
        value={draft[key]}
        onChange={event => setDraft({ ...draft, [key]: event.target.value.trim() })}
      />
    </label>
  );
  return (
    <section className="ctp-settings" aria-label={t("CTP 账户")}>
      {unreadable.map(entry => (
        <section className="ctp-account-card" key={entry.id} aria-label={entry.name}>
          <header>
            <h3>{entry.name}</h3>
          </header>
          <p role="alert">{t("连接文件无法读取，已保留原文件供检查")}</p>
        </section>
      ))}
      {connections.map(connection => (
        <section className="ctp-account-card" key={connection.id} aria-label={connection.name}>
          <header>
            <h3>{connection.name}</h3>
            {connection.id === snapshot?.ctp_market && (
              <span className="ctp-badge market">{t("用于行情")}</span>
            )}
            {connection.trading_record && <span className="ctp-badge">{t("已开通交易")}</span>}
          </header>
          <dl>
            <dt>{t("经纪商代码")}</dt>
            <dd>{connection.broker_id}</dd>
            <dt>{t("投资者账号")}</dt>
            <dd>{connection.user_id}</dd>
            <dt>AppID</dt>
            <dd>{connection.app_id}</dd>
            <dt>{t("交易前置")}</dt>
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
        <p className="workflow-empty">{t("还没有 CTP 账户。")}</p>
      )}
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
      {adding && (
        <AccountDialog
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
          {fixed && (
            <p className="subtle">
              {t("已开通交易，经纪商代码、投资者账号、AppID 和交易前置不能修改。")}
            </p>
          )}
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
        </AccountDialog>
      )}
      {!adding && error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
