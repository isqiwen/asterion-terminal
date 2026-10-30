import type { AccountMethod, CommandCall } from "../../src/bridge/commands";
import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { TradingConnection } from "../../src/bridge/client";
import { t, phases, type Run } from "./live-shared";
import { CreateLive } from "./CreateLive";
import { LiveAccount } from "./LiveAccount";

// Live CTP trading. Orders go to a real futures account only after the owner
// connects with credentials typed here, confirms the account, and the order
// passes the allowed contracts, exchange units and pre-trade risk.
export function LivePanel(context: TerminalContext) {
  const { snapshot, busy, trade } = context;
  // Every CTP account can trade; several may be open at once.
  const accounts = (snapshot?.trading_connections ?? []).filter(
    (item): item is TradingConnection => !("error" in item),
  );
  const [chosen, setChosen] = useWorkspaceDraft("live-account", "");
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  const [error, setError] = useState<DisplayError>("");
  // Commands always carry the account they were issued for.
  const runFor =
    (id: string): Run =>
    async (...[method, params]) => {
      setError("");
      try {
        // The tuple map guarantees each method's fields; this adapter adds the
        // one account it owns. Keep the identity last so callers cannot override it.
        await trade(...([method, { ...params, account: id }] as CommandCall<AccountMethod>));
        return true;
      } catch (reason) {
        setError(asDisplayError(reason));
        return false;
      }
    };
  const state = (item: TradingConnection) => {
    const session = snapshot?.live[item.id]?.session;
    if (!session) return snapshot?.live[item.id] ? "未配置风控" : "服务未连接";
    if (session.phase !== "ready") return phases[session.phase];
    return session.authorization ? "已允许发送委托" : "已就绪";
  };
  const entry = account ? snapshot?.live[account.id] : undefined;
  return (
    <section className="futures-data paper-trading" aria-label={t("CTP 交易账户")}>
      <div className="panel-heading">
        <h2>{t("CTP 交易")}</h2>
        <span className="panel-spacer" />
        <small>{t("柜台仿真 / 真实资金")}</small>
      </div>

      {error && !account && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {!accounts.length ? (
        <div className="workflow-empty">
          <p>{t("还没有交易连接，请先添加交易服务连接。")}</p>
          <button onClick={() => context.openSettings("ctp")}>{t("管理交易连接")}</button>
        </div>
      ) : (
        <div className="ctp-accounts">
          <nav className="ctp-account-list" aria-label={t("CTP 账户")}>
            {accounts.map(item => (
              <button
                key={item.id}
                aria-current={item.id === account?.id ? "true" : undefined}
                onClick={() => {
                  setError("");
                  setChosen(item.id);
                }}
              >
                <strong>{item.name}</strong>
                <span className="subtle">{t(state(item))}</span>
              </button>
            ))}
            <button className="ctp-account-manage" onClick={() => context.openSettings("ctp")}>
              {t("管理交易连接")}
            </button>
          </nav>
          {account && (
            <div className="ctp-account-panel" key={account.id}>
              {/* A refused command is reported in the panel of its account. */}
              {error && (
                <p className="alert" role="alert">
                  <ErrorNotice error={error} namespace="asterion.terminal.trading" />
                </p>
              )}
              {entry?.session ? (
                <LiveAccount
                  account={account}
                  live={entry.session}
                  connection={entry.connection}
                  snapshot={snapshot}
                  busy={busy}
                  run={runFor(account.id)}
                />
              ) : !entry ? (
                <div className="workflow-empty">
                  <p>
                    {account.name} · {account.host}:{account.port} · {account.instance}
                  </p>
                  <button
                    className="primary"
                    disabled={busy}
                    onClick={() => void runFor(account.id)("live.open")}
                  >
                    {t("连接交易服务")}
                  </button>
                  <p className="subtle">{t("连接交易服务以读取账户配置，不会自动登录柜台。")}</p>
                </div>
              ) : (
                <div className="account-setup">
                  <div className="workflow-heading">
                    <h3>{t("开通交易：{name}", { name: account.name })}</h3>
                  </div>
                  <CreateLive
                    account={account}
                    configuration={entry.account}
                    trade={trade}
                    snapshot={snapshot}
                    busy={busy}
                    run={runFor(account.id)}
                    openSettings={context.openSettings}
                  />
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </section>
  );
}
