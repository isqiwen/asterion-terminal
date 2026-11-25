import { useEffect, useRef, useState } from "react";
import { ActionError, useActionError } from "../../src/host/action-error";
import { ServiceStatusCard } from "../../src/host/components/ServiceStatusCard";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import type { TradingConnection } from "../../src/bridge/client";
import { BrokerState, Environment } from "../../src/generated/asterion/trading_pb";
import { useTradingAccounts, type TradingView } from "../../src/host/trading-accounts";
import { t, phases, type Run } from "./live-shared";
import { CreateLive } from "./CreateLive";
import { EmergencyControls } from "./EmergencyControls";
// Both trading views share one stylesheet; each loads it itself.
import "./trading.css";
import { LiveAccount } from "./LiveAccount";

export function accountState(view: TradingView | undefined) {
  if (!view) return "读取中";
  if (view.emergency) return "紧急控制模式";
  if (view.failed) return "服务未连接";
  if (!view.live) return "未配置风控";
  if (view.live.brokerState !== BrokerState.READY) return phases[view.live.brokerState] ?? "未连接";
  return view.live.authorizedMs !== undefined ? "已允许下单" : "已就绪";
}

export function AccountList({
  accounts,
  activeId,
  views,
  select,
}: {
  accounts: readonly TradingConnection[];
  activeId?: string;
  views: Readonly<Record<string, TradingView>>;
  select: (id: string) => void;
}) {
  return (
    <nav className="ctp-account-list" aria-label={t("CTP 账户")} hidden={accounts.length === 1}>
      {accounts.map(item => {
        const view = views[item.id];
        const real = view?.account?.environment === Environment.REAL;
        return (
          <button
            key={item.id}
            type="button"
            data-real={real || undefined}
            aria-current={item.id === activeId ? "true" : undefined}
            onClick={() => select(item.id)}
          >
            <strong>{item.instance}</strong>
            <span className="subtle">{t(accountState(view))}</span>
          </button>
        );
      })}
    </nav>
  );
}

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
  const paramAccount = context.workspaceParams?.account;
  const paramContract = context.workspaceParams?.contract;
  const [pendingContract, setPendingContract] = useState(paramContract);
  useEffect(() => {
    if (!paramAccount && !paramContract) return;
    if (paramAccount) setChosen(paramAccount);
    if (paramContract) setPendingContract(paramContract);
    context.navigate("workspace.trading", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramAccount, paramContract]);
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  const view = account ? views[account.id] : undefined;
  return (
    <section className="futures-data paper-trading" aria-label={t("CTP 交易账户")}>
      {!accounts.length ? (
        <ServiceUnset openSettings={context.openSettings} page="trading" />
      ) : (
        <div className="ctp-accounts" data-single={accounts.length === 1 || undefined}>
          {/* One account needs no list. */}
          <AccountList
            accounts={accounts}
            activeId={account?.id}
            views={views}
            select={setChosen}
          />
          {account && (
            <AccountColumn
              key={`${account.id}:${account.revision}`}
              account={account}
              view={view}
              send={send}
              context={context}
              initialContract={pendingContract}
              openStrategies={() =>
                context.navigate("workspace.trading-strategies", {
                  params: { account: account.id },
                })
              }
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
  send,
  context,
  initialContract,
  openStrategies,
}: {
  account: TradingConnection;
  view: TradingView | undefined;
  send: ReturnType<typeof useTradingAccounts>["send"];
  context: TerminalContext;
  initialContract?: string;
  openStrategies: () => void;
}) {
  const inFlight = useRef(false);
  const stoppingRef = useRef(false);
  const [busy, setBusy] = useState(false);
  const [stoppingStrategy, setStoppingStrategy] = useState(false);
  const { error, attempt } = useActionError();
  // Requests always carry the account they were issued for. A cancel and a
  // revoke take something back, so they do not wait for another request.
  const runFor =
    (hold: boolean): Run =>
    async operation => {
      if (hold) {
        if (inFlight.current) return false;
        inFlight.current = true;
        setBusy(true);
      }
      try {
        return await attempt(() =>
          send(account.id, typeof operation === "function" ? operation() : operation),
        );
      } finally {
        if (hold) {
          inFlight.current = false;
          setBusy(false);
        }
      }
    };
  const run = runFor(true);
  const control = runFor(false);
  const stopStrategy = async (id: string) => {
    if (stoppingRef.current) return false;
    stoppingRef.current = true;
    setStoppingStrategy(true);
    try {
      return await control({ case: "stopStrategy", value: { id } });
    } finally {
      stoppingRef.current = false;
      setStoppingStrategy(false);
    }
  };
  const instance = view?.strategies?.instance;
  return (
    <div className="ctp-account-column">
      <div className="ctp-account-panel">
        <ActionError error={error} namespace="asterion.terminal.trading" />
        {view?.emergency ? (
          <EmergencyControls
            account={account}
            state={view.emergency}
            real={view.account?.environment === Environment.REAL}
            control={control}
          />
        ) : view?.live && view.account ? (
          <LiveAccount
            account={account}
            record={view.account}
            stale={!!view.failed || view.live.recoveryRequired}
            context={context}
            busy={busy}
            run={run}
            control={control}
            initialContract={initialContract}
            strategy={instance}
            stoppingStrategy={stoppingStrategy}
            openStrategies={openStrategies}
            pauseStrategy={(id, paused) => run({ case: "pauseStrategy", value: { id, paused } })}
            flattenStrategy={id => run({ case: "flattenStrategy", value: { id } })}
            stopStrategy={stopStrategy}
          />
        ) : view?.account && view.initialized === false ? (
          <div className="account-setup">
            <div className="workflow-heading">
              <h2>{t("开通交易：{name}", { name: account.instance })}</h2>
            </div>
            <CreateLive
              account={account}
              configuration={view.account}
              busy={busy}
              run={runFor(true)}
            />
          </div>
        ) : (
          <ServiceStatusCard
            service="trading"
            title={account.instance}
            address={`${account.host}:${account.port}`}
            failed={view?.failed}
            openSettings={context.openSettings}
          />
        )}
      </div>
    </div>
  );
}
