import { useState } from "react";
import {
  ActionError,
  ErrorNotice,
  useActionError,
  useWorkspaceDraft,
  type TerminalContext,
} from "../contract";
import type { TradingConnection } from "../../src/bridge/client";
import { BrokerState } from "../../src/generated/asterion/trading_pb";
import { useTradingAccounts, type TradingView } from "../../src/host/trading-accounts";
import { t, phases, type Run } from "./live-shared";
import { StrategyPackages } from "./StrategyPackages";
import { CreateLive } from "./CreateLive";
import { LiveAccount } from "./LiveAccount";

// Live CTP trading. Orders go to a real futures account only after the owner
// confirms the account already connected by its host, and the order
// passes the allowed contracts, exchange units and pre-trade risk.
export function LivePanel(context: TerminalContext) {
  const { snapshot } = context;
  const { views, send } = useTradingAccounts();
  // Every CTP account can trade; several may be open at once.
  const accounts = (snapshot?.trading_connections ?? []).filter(
    (item): item is TradingConnection => !("error" in item),
  );
  const [chosen, setChosen] = useWorkspaceDraft("live-account", "");
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  const [busy, setBusy] = useState(false);
  const { error, attempt, clear } = useActionError();
  // Requests always carry the account they were issued for. A cancel and a
  // revoke take something back, so they do not wait for another request.
  const runFor =
    (id: string, hold: boolean): Run =>
    async operation => {
      if (hold) setBusy(true);
      const done = await attempt(() =>
        send(id, typeof operation === "function" ? operation() : operation),
      );
      if (hold) setBusy(false);
      return done;
    };
  const state = (view: TradingView | undefined) => {
    if (!view) return "读取中";
    if (view.failed) return "服务未连接";
    if (!view.live) return "未配置风控";
    if (view.live.brokerState !== BrokerState.READY)
      return phases[view.live.brokerState] ?? BrokerState[view.live.brokerState];
    return view.live.authorizedMs !== undefined ? "已允许发送委托" : "已就绪";
  };
  const repository = snapshot?.research_directory ?? "";
  const view = account ? views[account.id] : undefined;
  return (
    <section className="futures-data paper-trading" aria-label={t("CTP 交易账户")}>
      <div className="panel-heading">
        <h2>{t("CTP 交易")}</h2>
        <span className="panel-spacer" />
        <small>{t("柜台仿真 / 真实资金")}</small>
      </div>
      {!accounts.length ? (
        <div className="workspace-empty">
          <strong>{t("还没有交易连接")}</strong>
          <p>{t("在设置里添加交易主机后，这里显示账户、委托和策略。")}</p>
          <button className="primary" onClick={() => context.openSettings("trading")}>
            {t("管理交易连接")}
          </button>
        </div>
      ) : (
        <div className="ctp-accounts">
          <nav className="ctp-account-list" aria-label={t("CTP 账户")}>
            {accounts.map(item => (
              <button
                key={item.id}
                aria-current={item.id === account?.id ? "true" : undefined}
                onClick={() => {
                  setChosen(item.id);
                  clear();
                }}
              >
                <strong>{item.instance}</strong>
                <span className="subtle">{t(state(views[item.id]))}</span>
              </button>
            ))}
            <button className="ctp-account-manage" onClick={() => context.openSettings("trading")}>
              {t("管理交易连接")}
            </button>
          </nav>
          {account && (
            <div className="ctp-account-column" key={`${account.id}:${account.revision}`}>
              <div className="ctp-account-panel">
                <ActionError error={error} namespace="asterion.terminal.trading" />
                {view?.live && view.account ? (
                  <LiveAccount
                    account={account}
                    record={view.account}
                    stale={!!view.failed || view.live.recoveryRequired}
                    snapshot={snapshot}
                    busy={busy}
                    run={runFor(account.id, true)}
                    control={runFor(account.id, false)}
                  />
                ) : view?.account && view.initialized === false ? (
                  <div className="account-setup">
                    <div className="workflow-heading">
                      <h3>{t("开通交易：{name}", { name: account.instance })}</h3>
                    </div>
                    <CreateLive
                      account={account}
                      configuration={view.account}
                      busy={busy}
                      run={runFor(account.id, true)}
                      openSettings={context.openSettings}
                    />
                  </div>
                ) : (
                  <div className="workflow-empty">
                    <p>
                      {account.instance} · {account.host}:{account.port}
                    </p>
                    {view?.failed ? (
                      <p className="alert" role="alert">
                        <ErrorNotice error={view.failed} namespace="asterion.terminal.trading" />
                      </p>
                    ) : (
                      <p role="status">{t("正在读取交易服务…")}</p>
                    )}
                  </div>
                )}
              </div>
              <div className="ctp-account-panel">
                <StrategyPackages
                  account={account.id}
                  repository={repository}
                  record={view?.account}
                  connected={!!view && !view.failed}
                />
              </div>
            </div>
          )}
        </div>
      )}
    </section>
  );
}
