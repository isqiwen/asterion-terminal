import type { TradingConnection } from "../../src/bridge/client";
import { Environment } from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useTradingAccounts } from "../../src/host/trading-accounts";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { t } from "./live-shared";
import { StrategyInstance } from "./StrategyInstance";
import { StrategyMonitor } from "./StrategyMonitor";
import { StrategyPackages } from "./StrategyPackages";
import { useStrategyHost } from "./strategy-host";

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
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  return (
    <section className="futures-data paper-trading" aria-label={t("交易策略")}>
      {accounts.length > 1 && (
        <div className="panel-heading">
          <div className="tabs" role="group" aria-label={t("CTP 账户")}>
            {accounts.map(item => (
              <button
                key={item.id}
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
        />
      ) : (
        <div className="workspace-empty">
          <strong>{t("还没有交易连接")}</strong>
          <button className="primary" onClick={() => context.openSettings("trading")}>
            {t("添加交易连接")}
          </button>
        </div>
      )}
    </section>
  );
}

function AccountStrategies({
  account,
  repository,
}: {
  account: TradingConnection;
  repository: string;
}) {
  const host = useStrategyHost(account.id);
  const view = useTradingAccounts().views[account.id];
  const instance = host.state?.instance;
  const broker = view?.account?.broker;
  const real = view?.account?.environment === Environment.REAL;
  return (
    <div className="ctp-strategy-body">
      <div className="ctp-account-panel">
        <div className="ctp-identity" data-real={real || undefined}>
          <strong>
            {[account.instance, broker?.brokerId, broker?.userId].filter(Boolean).join(" · ")}
          </strong>
          {view?.account && (
            <StatusBadge tone={real ? "danger" : "neutral"}>
              {t(real ? "真实资金" : "CTP 仿真")}
            </StatusBadge>
          )}
        </div>
        {instance?.instance && (
          <StrategyMonitor
            instance={instance}
            positions={view?.live?.positions ?? []}
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
            state={host.state}
            account={view?.account}
            available={host.fresh && !!view && !view.failed}
            busy={host.busy || host.stopping}
            start={configuration => host.mutate({ case: "startStrategy", value: configuration })}
          />
        )}
      </div>
      <div className="ctp-account-panel">
        <StrategyPackages host={host} repository={repository} />
      </div>
    </div>
  );
}
