import { useEffect, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import { BrokerState, Environment } from "../../src/generated/asterion/trading_pb";
import { activeStrategy } from "./strategy-instance";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useStrategyHost, useTradingAccounts } from "../../src/host/trading-accounts";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { ActionError, useActionError } from "../../src/host/action-error";
import { AccountIdentityBar } from "./AccountIdentityBar";
import { EmergencyControls } from "./EmergencyControls";
import { AccountList, TradingStatusCard } from "./LivePanel";
import { t, type Run } from "./live-shared";
import { StrategyInstance } from "./StrategyInstance";
import { StrategyMonitor } from "./StrategyMonitor";
// Both trading views share one stylesheet; each loads it itself.
import "./trading.css";
import { StrategyPackages } from "./StrategyPackages";

// Strategies on the Trading hosts, apart from manual trading, as vn.py's CTA
// module and QuantConnect's live page keep them: an account's running instance
// with its targets beside the actual positions, or what starts one, and the
// packages that can run.
export function LiveStrategies(context: TerminalContext) {
  const accounts = (context.snapshot?.trading_connections ?? []).filter(
    (item): item is TradingConnection => !("error" in item),
  );
  // The account view and this one show the same account.
  const [chosen, setChosen] = useWorkspaceDraft("live-account", "");
  const paramAccount = context.workspaceParams?.account;
  const paramStrategy = context.workspaceParams?.strategy;
  const paramRun = context.workspaceParams?.run;
  const [pending, setPending] = useState<{ strategy?: string; run?: string }>({
    strategy: paramStrategy,
    run: paramRun,
  });
  useEffect(() => {
    if (!paramAccount && !paramStrategy && !paramRun) return;
    if (paramAccount) setChosen(paramAccount);
    if (paramStrategy || paramRun) setPending({ strategy: paramStrategy, run: paramRun });
    context.navigate("workspace.trading-strategies", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramAccount, paramStrategy, paramRun]);
  const { views } = useTradingAccounts();
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  return (
    <section className="futures-data paper-trading" aria-label={t("交易策略")}>
      {account ? (
        <div className="ctp-accounts" data-single={accounts.length === 1 || undefined}>
          <AccountList accounts={accounts} activeId={account.id} views={views} select={setChosen} />
          <AccountStrategies
            key={`${account.id}:${account.revision}`}
            account={account}
            repository={context.snapshot?.research_directory ?? ""}
            initialStrategy={pending.strategy}
            initialRun={pending.run}
            openWorkbench={() =>
              context.navigate("workspace.trading", { params: { account: account.id } })
            }
            openSettings={() => context.openSettings("trading")}
            openRun={id => context.navigate("workspace.lab-runs", { params: { run: id } })}
            reconcileInstance={(accountId, instanceId, runId) =>
              context.navigate("workspace.lab-runs", {
                params: {
                  reconcileAccount: accountId,
                  reconcileInstance: instanceId,
                  ...(runId ? { reconcileRun: runId } : {}),
                },
              })
            }
          />
        </div>
      ) : (
        <ServiceUnset openSettings={context.openSettings} page="trading" />
      )}
    </section>
  );
}

function AccountStrategies({
  account,
  repository,
  initialStrategy,
  initialRun,
  openWorkbench,
  openSettings,
  openRun,
  reconcileInstance,
}: {
  account: TradingConnection;
  repository: string;
  initialStrategy?: string;
  initialRun?: string;
  openWorkbench: () => void;
  openSettings: () => void;
  openRun: (id: string) => void;
  reconcileInstance: (accountId: string, instanceId: string, runId?: string) => void;
}) {
  const host = useStrategyHost(account.id);
  const [published, setPublished] = useState("");
  const [deployFirst, setDeployFirst] = useState(false);
  const { views, send } = useTradingAccounts();
  const view = views[account.id];
  const { error: controlError, attempt } = useActionError();
  const controlRun: Run = operation =>
    attempt(() => send(account.id, typeof operation === "function" ? operation() : operation));
  if (view?.emergency) {
    return (
      <div className="ctp-strategy-body">
        <div className="ctp-account-panel">
          <ActionError error={controlError} namespace="asterion.terminal.trading" />
          <EmergencyControls
            account={account}
            state={view.emergency}
            real={view.account?.environment === Environment.REAL}
            control={controlRun}
          />
        </div>
      </div>
    );
  }
  if (!host.state) {
    return (
      <div className="ctp-account-column">
        <div className="ctp-account-panel">
          <TradingStatusCard
            account={account}
            failed={view?.failed || host.readError}
            openSettings={openSettings}
          />
        </div>
      </div>
    );
  }
  const instance = host.state.instance;
  const live = view?.live;
  const stale = !!view?.failed || !!live?.recoveryRequired;
  const runningUnauthorized =
    activeStrategy(instance?.state) && !!live && live.authorizedMs === undefined;
  const needsAuthorize =
    live?.brokerState === BrokerState.READY && !stale && live.authorizedMs === undefined;
  return (
    <div className="ctp-strategy-body" data-running={activeStrategy(instance?.state) || undefined}>
      <div className="ctp-account-panel">
        <AccountIdentityBar
          account={account}
          record={view?.account}
          stale={stale}
          control={controlRun}
          extraActions={
            needsAuthorize ? (
              <button type="button" onClick={openWorkbench}>
                {t("前往允许下单")}
              </button>
            ) : undefined
          }
        />
        <ActionError error={controlError} namespace="asterion.terminal.trading" />
        {runningUnauthorized && (
          <div className="alert ctp-strategy-unauthorized" role="alert">
            <span>{t("策略已运行，但账户尚未允许下单：目标仅记录不发送。")}</span>
            <button type="button" className="primary" onClick={openWorkbench}>
              {t("前往允许下单")}
            </button>
          </div>
        )}
        {instance?.instance && (
          <StrategyMonitor
            instance={instance}
            account={view}
            observedAt={host.observedAt}
            fresh={host.fresh}
            stopping={host.stopping}
            busy={host.busy}
            reconcile={() => {
              const cfg = instance.instance!;
              const matched = host.state?.packages.find(
                pkg => pkg.name === cfg.packageName && pkg.version === cfg.packageVersion,
              );
              reconcileInstance(account.id, cfg.id, matched?.research?.runId || undefined);
            }}
            stop={id => host.mutate({ case: "stopStrategy", value: { id } })}
            pause={(id, paused) => host.mutate({ case: "pauseStrategy", value: { id, paused } })}
            flatten={id => host.mutate({ case: "flattenStrategy", value: { id } })}
          />
        )}
        {!instance?.instance && !host.state.packages.length ? (
          <div className="workspace-empty">
            <div className="trading-empty-card">
              <div className="trading-empty-head">
                <strong>
                  {t(view?.initialized === false ? "尚未开通交易" : "尚未部署策略版本")}
                </strong>
              </div>
              <p>
                {t(
                  view?.initialized === false
                    ? "可在右侧先部署策略版本；启动运行前需先在工作台完成账户开通与风控设置。"
                    : "请先在右侧「策略版本」选择策略并部署，部署完成后即可在此配置并启动实例。",
                )}
              </p>
              {view?.initialized === false && (
                <div className="workspace-empty-actions">
                  <button type="button" className="primary" onClick={openWorkbench}>
                    {t("前往开通交易")}
                  </button>
                </div>
              )}
            </div>
          </div>
        ) : (
          <StrategyInstance
            connection={account.id}
            repository={repository}
            state={host.state}
            account={view?.account}
            available={host.fresh && !!view && !view.failed}
            busy={host.busy || host.stopping}
            published={published}
            deployFirst={deployFirst}
            start={configuration => host.mutate({ case: "startStrategy", value: configuration })}
          />
        )}
      </div>
      <div className="ctp-account-panel ctp-strategy-versions">
        <StrategyPackages
          key={`${repository}:${initialStrategy ?? ""}:${initialRun ?? ""}`}
          host={host}
          repository={repository}
          initialStrategy={initialStrategy}
          initialRun={initialRun}
          openRun={openRun}
          published={setPublished}
          onDeployFirst={setDeployFirst}
        />
      </div>
    </div>
  );
}
