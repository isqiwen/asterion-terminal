import { useEffect, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import { Environment } from "../../src/generated/asterion/trading_pb";
import { activeStrategy } from "./strategy-instance";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useStrategyHost, useTradingAccounts } from "../../src/host/trading-accounts";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { Tag } from "../../src/ui/Tag";
import { ActionError, useActionError } from "../../src/host/action-error";
import { EmergencyControls } from "./EmergencyControls";
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
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  return (
    <section className="futures-data paper-trading" aria-label={t("交易策略")}>
      {accounts.length > 1 && (
        <div className="panel-heading">
          <div className="tabs" role="group" aria-label={t("CTP 账户")}>
            {accounts.map(item => (
              <button
                key={item.id}
                type="button"
                aria-pressed={item.id === account?.id}
                onClick={() => setChosen(item.id)}
              >
                {item.instance}
              </button>
            ))}
          </div>
        </div>
      )}
      {account ? (
        <AccountStrategies
          key={`${account.id}:${account.revision}`}
          account={account}
          repository={context.snapshot?.research_directory ?? ""}
          initialStrategy={pending.strategy}
          initialRun={pending.run}
          openRun={id => context.navigate("workspace.lab-runs", { params: { run: id } })}
        />
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
  openRun,
}: {
  account: TradingConnection;
  repository: string;
  initialStrategy?: string;
  initialRun?: string;
  openRun: (id: string) => void;
}) {
  const host = useStrategyHost(account.id);
  const [published, setPublished] = useState("");
  const [deployFirst, setDeployFirst] = useState(false);
  const { views, send } = useTradingAccounts();
  const view = views[account.id];
  const { error: emergencyError, attempt } = useActionError();
  const emergencyRun: Run = operation =>
    attempt(() => send(account.id, typeof operation === "function" ? operation() : operation));
  if (view?.emergency) {
    return (
      <div className="ctp-strategy-body">
        <div className="ctp-account-panel">
          <ActionError error={emergencyError} namespace="asterion.terminal.trading" />
          <EmergencyControls account={account} state={view.emergency} control={emergencyRun} />
        </div>
      </div>
    );
  }
  const instance = host.state?.instance;
  const broker = view?.account?.broker;
  const real = view?.account?.environment === Environment.REAL;
  return (
    <div className="ctp-strategy-body" data-running={activeStrategy(instance?.state) || undefined}>
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
      <div className="ctp-account-panel">
        <div className="ctp-identity" data-real={real || undefined}>
          <strong>
            {[account.instance, broker?.brokerId, broker?.userId].filter(Boolean).join(" · ")}
          </strong>
          {view?.account && <Tag danger={real}>{t(real ? "真实资金" : "CTP 仿真")}</Tag>}
        </div>
        {instance?.instance && (
          <StrategyMonitor
            instance={instance}
            account={view}
            observedAt={host.observedAt}
            fresh={host.fresh}
            stopping={host.stopping}
            busy={host.busy}
            stop={id => host.mutate({ case: "stopStrategy", value: { id } })}
            pause={(id, paused) => host.mutate({ case: "pauseStrategy", value: { id, paused } })}
            flatten={id => host.mutate({ case: "flattenStrategy", value: { id } })}
          />
        )}
        {host.state && (
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
    </div>
  );
}
