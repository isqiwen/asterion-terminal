import { listSeparator } from "../../src/i18n";
import { ctpErrorSummary } from "../../src/i18n/errors";
import { useEffect, useRef, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import {
  BrokerState,
  Environment,
  type Account,
  type InstanceStatus,
} from "../../src/generated/asterion/trading_pb";
import { FlowSteps } from "../../src/ui/FlowSteps";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarket, useMarketAvailable } from "../../src/host/market";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { AccountIdentityBar } from "./AccountIdentityBar";
import { AccountRecords } from "./AccountRecords";
import { AccountSummary } from "./AccountSummary";
import { type Activity } from "./ActivityTabs";
import { OrderTicket, type Preset } from "./OrderTicket";
import { StrategyCard } from "./StrategyCard";
import { TradeMarket } from "./TradeMarket";
import { activeStrategy } from "./strategy-instance";
import { explicitCloseBuckets } from "../../src/ui/closePolicy";
import {
  t,
  phases,
  command,
  environmentName,
  formatRiskBreach,
  type CommandAction,
  type Run,
} from "./live-shared";

function LiveTradeMarket({
  context,
  session,
  contract,
  select,
}: {
  context: TerminalContext;
  session: Account["session"];
  contract: string;
  select: (contract: string) => void;
}) {
  const market = useMarket();
  if (!market || !session) return null;
  return (
    <TradeMarket
      context={context}
      market={market}
      session={session}
      contract={contract}
      select={select}
    />
  );
}

export function LiveAccount({
  account,
  record,
  stale,
  context,
  busy,
  run,
  control,
  initialContract,
  strategy,
  stoppingStrategy = false,
  openStrategies,
  pauseStrategy,
  flattenStrategy,
  stopStrategy,
}: {
  account: TradingConnection;
  // The account as Trading reports it, with its open session.
  record: Account;
  // The service is unreachable or its record needs recovery.
  stale: boolean;
  context: TerminalContext;
  busy: boolean;
  run: Run;
  control: Run;
  initialContract?: string;
  strategy?: InstanceStatus;
  stoppingStrategy?: boolean;
  openStrategies?: () => void;
  pauseStrategy?: (id: string, paused: boolean) => Promise<boolean>;
  flattenStrategy?: (id: string) => Promise<boolean>;
  stopStrategy?: (id: string) => Promise<boolean>;
}) {
  const [activity, setActivity] = useState<Activity>("positions");
  const [preset, setPreset] = useState<Preset>();
  const { snapshot } = context;
  const hasMarket = useMarketAvailable();
  // One contract across the list, the chart and the order ticket.
  const [contract, setContract] = useWorkspaceDraft(
    `trade-contract:${account.id}`,
    initialContract ?? "",
  );
  useEffect(() => {
    if (initialContract) setContract(initialContract);
  }, [initialContract, setContract]);
  const live = record.session!;
  const broker = record.broker!;
  const identity = JSON.stringify([
    account.id,
    account.host,
    account.port,
    account.instance,
    broker.front,
    broker.brokerId,
    broker.userId,
  ]);
  const real = record.environment === Environment.REAL;
  const [environmentConfirmed, setEnvironmentConfirmed] = useWorkspaceDraft(
    `ctp-environment-confirmed:${identity}`,
    false,
  );
  // Real-money accounts confirm the broker environment in each window before connecting or trading.
  const environmentReady = !real || environmentConfirmed;

  const [confirmedPolicy, setConfirmedPolicy] = useState<string | null>(null);
  const confirmed = confirmedPolicy === live.policyRevision;
  const ready = live.brokerState === BrokerState.READY;
  const authorized = live.authorizedMs !== undefined;
  const strategyRunning = !!(strategy?.instance && activeStrategy(strategy.state));
  const act = (action: () => CommandAction) => command(run, record, action);
  // One step is offered at a time: the counter connection, then permission to
  // send, then the order ticket. A window that has not confirmed the
  // environment of an already connected account confirms it first.
  const stage = stale
    ? "stale"
    : live.brokerState === BrokerState.DISCONNECTED || live.brokerState === BrokerState.FAILED
      ? "connect"
      : !ready
        ? "connecting"
        : !environmentReady
          ? "environment"
          : !authorized
            ? "authorize"
            : "trade";
  const environmentFields = (
    <div className="environment-check">
      <label>
        {t("柜台环境")}
        <span>{t(environmentName(record.environment))}</span>
      </label>
      <label className="checkbox">
        <input
          type="checkbox"
          required
          checked={environmentConfirmed}
          onChange={e => setEnvironmentConfirmed(e.target.checked)}
        />
        {t("我已核对账户与柜台环境")}
      </label>
    </div>
  );
  // What takes something back is never held back by another command, of this
  // account or of the window: a cancel is disabled only while the same one is on its way.
  const inFlightCancels = useRef(new Set<string>());
  const [cancelling, setCancelling] = useState<readonly string[]>([]);
  const cancel = async (id: string) => {
    if (inFlightCancels.current.has(id)) return;
    inFlightCancels.current.add(id);
    setCancelling(current => [...current, id]);
    try {
      await command(control, record, () => ({ case: "cancelOrder", value: { id } }));
    } finally {
      inFlightCancels.current.delete(id);
      setCancelling(current => current.filter(item => item !== id));
    }
  };
  return (
    <>
      <AccountIdentityBar
        account={account}
        record={record}
        stale={stale}
        environmentReady={environmentReady}
        busy={busy}
        run={run}
        control={control}
      />
      {stale && (
        <p className="alert" role="alert">
          {t("交易服务失联，显示的是最后读到的状态")}
        </p>
      )}
      {live.brokerState === BrokerState.FAILED && (
        <p className="alert" role="alert">
          {live.ctpError === -1008
            ? t("柜台回报队列已满，已停止报单；重连后核对账户")
            : ctpErrorSummary(live.ctpError)}
        </p>
      )}
      {live.riskBreaches.length > 0 && (
        <p className="alert" role="alert">
          {t("硬风控已触发，暂停新增开仓：{items}", {
            items: live.riskBreaches.map(formatRiskBreach).join(listSeparator()),
          })}
        </p>
      )}
      {live.unconfirmed.length > 0 && (
        <section className="alert" role="alert" aria-label={t("未确认委托")}>
          <p>
            {t("{n} 笔委托未出现在柜台回报，不会重发，仍占用在途额度：{ids}", {
              n: live.unconfirmed.length,
              ids: live.unconfirmed.map(item => item.id).join(listSeparator()),
            })}
          </p>
          <div className="source-actions">
            {live.unconfirmed.map(item => (
              <button
                key={item.id}
                type="button"
                disabled={busy || stale}
                onClick={() => void act(() => ({ case: "resolveOrder", value: { id: item.id } }))}
              >
                {t("已核实券商无此委托：{id}", { id: item.id })}
              </button>
            ))}
          </div>
        </section>
      )}
      <AccountSummary live={live} />
      <div className="ctp-trade-body" data-market={hasMarket || undefined}>
        {hasMarket && (
          <LiveTradeMarket
            context={context}
            session={live}
            contract={contract}
            select={setContract}
          />
        )}
        <div className="ctp-trade-records">
          <AccountRecords
            connection={account.id}
            record={record}
            busy={busy}
            run={run}
            activity={activity}
            setActivity={setActivity}
            ready={ready}
            stale={stale}
            cancelling={cancelling}
            cancel={cancel}
            close={
              stage === "trade" && !strategyRunning
                ? next => {
                    setPreset(next);
                  }
                : undefined
            }
            separate={exchange => !!explicitCloseBuckets(snapshot, exchange)}
          />
        </div>
        <aside className="ctp-trade-side" aria-label={t(strategyRunning ? "运行中的策略" : "下单")}>
          {stage !== "stale" && stage !== "trade" && (
            <FlowSteps
              labels={[
                t(real ? "核对并连接" : "连接账户"),
                t("允许下单"),
                t(strategyRunning ? "策略执行" : "下单"),
              ]}
              current={stage === "authorize" ? 1 : 0}
            />
          )}
          {stage === "connect" && (
            <form
              aria-label={t("连接账户")}
              onSubmit={event => {
                event.preventDefault();
                if (!environmentReady) return;
                void run({ case: "connectBroker", value: {} });
              }}
            >
              <fieldset disabled={busy}>
                {real && environmentFields}
                <div className="source-actions">
                  <button className="primary" type="submit" disabled={!environmentReady}>
                    {t("连接账户")}
                  </button>
                </div>
              </fieldset>
            </form>
          )}
          {stage === "connecting" && (
            <p role="status">
              {t("正在连接柜台：{phase}", { phase: t(phases[live.brokerState] ?? "未连接") })}
            </p>
          )}
          {stage === "environment" && (
            <section aria-label={t("柜台环境")}>{environmentFields}</section>
          )}
          {stage === "authorize" && (
            <section className="trading-permission" aria-label={t("允许下单")}>
              <label className="checkbox">
                <input
                  type="checkbox"
                  checked={confirmed}
                  onChange={event =>
                    setConfirmedPolicy(event.target.checked ? live.policyRevision : null)
                  }
                />
                {t("我确认使用账户 {user} 向上方柜台发送委托", { user: broker.userId })}
              </label>
              <div className="source-actions">
                <button
                  type="button"
                  className="primary"
                  title={t("停止下单、修改风控设置或交易服务重启后失效")}
                  disabled={busy || !confirmed}
                  onClick={() => {
                    setConfirmedPolicy(null);
                    void act(() => ({ case: "authorize", value: { userId: broker.userId } }));
                  }}
                >
                  {t("允许下单")}
                </button>
              </div>
            </section>
          )}
          {strategyRunning && openStrategies && (
            <StrategyCard
              instance={strategy!}
              live={live}
              open={openStrategies}
              busy={busy}
              stopping={stoppingStrategy}
              pause={pauseStrategy}
              flatten={flattenStrategy}
              stop={stopStrategy}
            />
          )}
          {stage === "trade" && !strategyRunning && (
            <OrderTicket
              connection={account.id}
              record={record}
              snapshot={snapshot}
              busy={busy}
              accountName={account.instance}
              confirm={real}
              run={run}
              onSent={() => setActivity("orders")}
              preset={preset}
              onPresetApplied={() => setPreset(undefined)}
              initialContract={contract || initialContract}
              onContract={setContract}
            />
          )}
        </aside>
      </div>
    </>
  );
}
