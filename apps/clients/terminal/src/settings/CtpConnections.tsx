import { useState } from "react";
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
// One place for the counter accounts the broker issues. One of them is
// current: the market login, the contract catalog query and trading use it.
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
  const field = (
    key: keyof typeof blank,
    label: string,
    options: { required?: boolean; maxLength: number; placeholder?: string },
  ) => (
    <label>
      {label}
      <input
        aria-label={label}
        required={options.required}
        maxLength={options.maxLength}
        placeholder={options.placeholder}
        value={draft[key]}
        onChange={event => setDraft({ ...draft, [key]: event.target.value.trim() })}
      />
    </label>
  );
  return (
    <section className="native-plugin-service" aria-label={t("CTP 账户")}>
      <h2>{t("CTP 账户")}</h2>
      <p>
        {t(
          "在这里填写一次期货公司提供的柜台信息。行情和交易都使用当前账户；密码与授权码在每次连接时输入，不保存。",
        )}
      </p>
      {unreadable.map(entry => (
        <section key={entry.id} aria-label={entry.name}>
          <h3>{entry.name}</h3>
          <p role="alert">{t("连接文件无法读取，已保留原文件供检查")}</p>
        </section>
      ))}
      {connections.map(connection => (
        <section key={connection.id} aria-label={connection.name}>
          <h3>
            {connection.name}
            {connection.id === snapshot?.ctp_current && ` · ${t("当前账户")}`}
          </h3>
          <p>
            {t("经纪商代码")} {connection.broker_id} · {t("投资者账号")} {connection.user_id}
          </p>
          <p>
            {t("交易前置")} {connection.trade_front || t("未填写")} · {t("行情前置")}{" "}
            {connection.market_front || t("未填写")}
          </p>
          {connection.id !== snapshot?.ctp_current && (
            <>
              <button
                disabled={busy}
                onClick={() => void run("ctp.connections.select", { id: connection.id })}
              >
                {t("设为当前账户")}
              </button>{" "}
            </>
          )}
          <button disabled={busy} onClick={() => edit(connection)}>
            {t("编辑账户")}
          </button>{" "}
          {removing === connection.id ? (
            <button
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
        </section>
      ))}
      <form
        onSubmit={async event => {
          event.preventDefault();
          if (
            await run("ctp.connections.save", {
              id: editing?.id ?? `ctp-${crypto.randomUUID()}`,
              revision: editing?.revision ?? "",
              ...draft,
              name: draft.name.trim(),
            })
          )
            edit(null);
        }}
      >
        <h3>{t(editing ? "编辑账户" : "新建账户")}</h3>
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
          {field("broker_id", t("经纪商代码"), { required: true, maxLength: 10 })}
          {field("user_id", t("投资者账号"), { required: true, maxLength: 15 })}
          {field("app_id", "AppID", { maxLength: 32 })}
          {field("trade_front", t("交易前置"), { maxLength: 64, placeholder: "tcp://host:port" })}
          {field("market_front", t("行情前置"), { maxLength: 64, placeholder: "tcp://host:port" })}
        </fieldset>
        <button disabled={busy || (!draft.trade_front && !draft.market_front)}>
          {t("保存账户")}
        </button>{" "}
        {editing && (
          <button type="button" disabled={busy} onClick={() => edit(null)}>
            {t("取消编辑")}
          </button>
        )}
      </form>
      <p>
        {t(
          "只看行情可以只填行情前置；交易和合约目录查询需要交易前置与 AppID。切换当前账户前先断开行情并关闭交易账户。修改或删除不影响已创建的交易记录，它保留创建时的柜台信息。",
        )}
      </p>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
