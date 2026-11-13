import { useState } from "react";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type DisplayError, ErrorNotice } from "../../src/i18n/errors";
import type { TradingConnection } from "../../src/bridge/client";
import { BrokerState } from "../../src/generated/asterion/trading_pb";
import { useTradingAccounts, type TradingView } from "../../src/host/trading-accounts";
import { t, phases, type Run } from "./live-shared";
import { useStrategyHost } from "./strategy-host";
import { activeStrategy } from "./strategy-instance";
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
  const view = account ? views[account.id] : undefined;
  return (
    <section className="futures-data paper-trading" aria-label={t("CTP 交易账户")}>
      <div className="panel-heading">
        <h2>{t("CTP 交易")}</h2>
      </div>
      {!accounts.length ? (
        <div className="workspace-empty">
          <strong>{t("还没有交易连接")}</strong>
          <p>{t("在设置里添加交易连接后，这里显示账户、委托和策略。")}</p>
          <button className="primary" onClick={() => context.openSettings("trading")}>
            {t("添加交易连接")}
          </button>
        </div>
      ) : (
        <div className="ctp-accounts" data-single={accounts.length === 1 || undefined}>
          {/* One account needs no list. */}
          <nav
            className="ctp-account-list"
            aria-label={t("CTP 账户")}
            hidden={accounts.length === 1}
          >
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
          </nav>
          {account && (
            <AccountColumn
              key={`${account.id}:${account.revision}`}
              account={account}
              view={view}
              error={error}
              snapshot={snapshot}
              busy={busy}
              runFor={runFor}
              openStrategies={() => context.navigate("workspace.trading-strategies")}
            />
          )}
        </div>
      )}
    </section>
  );
}

// One account's trading panel. A running strategy is named above it: manual
// orders wait while it runs, and its view has the controls.
function AccountColumn({
  account,
  view,
  error,
  snapshot,
  busy,
  runFor,
  openStrategies,
}: {
  account: TradingConnection;
  view: TradingView | undefined;
  error: DisplayError;
  snapshot: TerminalContext["snapshot"];
  busy: boolean;
  runFor: (id: string, hold: boolean) => Run;
  openStrategies: () => void;
}) {
  const instance = useStrategyHost(account.id).state?.instance;
  return (
    <div className="ctp-account-column">
      {instance?.instance && activeStrategy(instance.state) && (
        <p className="ctp-strategy-notice" role="status">
          {t("策略实例 {id} 正在运行", { id: instance.instance.id })}{" "}
          <button className="link" onClick={openStrategies}>
            {t("查看策略")}
          </button>
        </p>
      )}
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
    </div>
  );
}
