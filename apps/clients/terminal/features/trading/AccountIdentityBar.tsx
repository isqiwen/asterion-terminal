import { useRef, useState, type ReactNode } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import { BrokerState, Environment, type Account } from "../../src/generated/asterion/trading_pb";
import { ctpErrorSummary } from "../../src/i18n/errors";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { Tag } from "../../src/ui/Tag";
import { command, phases, t, type Run } from "./live-shared";

// The counter link: ready, on its way, or down.
function phaseTone(stale: boolean, state: BrokerState): Tone {
  if (stale || state === BrokerState.FAILED || state === BrokerState.DISCONNECTED) return "danger";
  return state === BrokerState.READY ? "ok" : "attention";
}

// Persistent account header shared by Workbench and Strategies so environment,
// connection, reconciliation, order authorization and emergency revoke always
// sit in the same place.
export function AccountIdentityBar({
  account,
  record,
  stale,
  environmentReady = true,
  busy = false,
  run,
  control,
  extraActions,
}: {
  account: TradingConnection;
  record?: Account;
  stale: boolean;
  environmentReady?: boolean;
  busy?: boolean;
  run?: Run;
  control?: Run;
  extraActions?: ReactNode;
}) {
  const live = record?.session;
  const broker = record?.broker;
  const real = record?.environment === Environment.REAL;
  const ready = live?.brokerState === BrokerState.READY;
  const authorized = live?.authorizedMs !== undefined;
  const revokingRef = useRef(false);
  const [revoking, setRevoking] = useState(false);

  return (
    <div className="ctp-identity" data-real={real || undefined}>
      <div className="ctp-identity-main">
        <strong title={broker?.front}>
          {[account.instance, broker?.brokerId, broker?.userId].filter(Boolean).join(" · ")}
        </strong>
        {record && <Tag danger={real}>{t(real ? "真实资金" : "CTP 仿真")}</Tag>}
        {live && !environmentReady && (
          <StatusBadge tone="attention">{t("CTP 环境待确认")}</StatusBadge>
        )}
        {live && (
          <span
            title={
              stale && !live.recoveryRequired
                ? t("交易服务失联，显示的是最后读到的状态")
                : !stale && live.brokerState === BrokerState.FAILED && live.ctpError
                  ? ctpErrorSummary(live.ctpError)
                  : undefined
            }
          >
            <StatusBadge tone={phaseTone(stale, live.brokerState)}>
              <span data-testid="live-phase">
                {t(
                  live.recoveryRequired
                    ? "需恢复"
                    : stale
                      ? "服务失联"
                      : (phases[live.brokerState] ?? "未连接"),
                )}
              </span>
            </StatusBadge>
          </span>
        )}
        {live && (
          <span
            title={t(
              "资金、持仓、订单和成交可能来自不同时间；同步完成时间不代表原子账本或完整历史。",
            )}
          >
            <StatusBadge tone={live.positionsReconciled ? "neutral" : "attention"}>
              {t(live.positionsReconciled ? "持仓已核对" : "持仓待核对")}
            </StatusBadge>
          </span>
        )}
      </div>
      {/* The account's actions stay together when the line wraps. */}
      <div className="ctp-identity-actions">
        {live && (
          <strong
            className="trading-switch"
            role="status"
            aria-label={t("下单状态")}
            data-state={authorized ? (ready && !stale ? "on" : "waiting") : "off"}
          >
            {t(authorized ? "已允许下单" : "未允许下单")}
          </strong>
        )}
        {live && record && authorized && control && (
          <button
            type="button"
            className="danger"
            disabled={revoking}
            onClick={() => {
              if (revokingRef.current) return;
              revokingRef.current = true;
              setRevoking(true);
              void command(control, record, () => ({ case: "revoke", value: {} })).finally(() => {
                revokingRef.current = false;
                setRevoking(false);
              });
            }}
          >
            {t("停止下单")}
          </button>
        )}
        {live && run && live.brokerState !== BrokerState.DISCONNECTED && (
          <button
            type="button"
            disabled={busy}
            title={t("断开账户会退出柜台连接，已有委托不会自动撤销。")}
            onClick={() => void run({ case: "disconnectBroker", value: {} })}
          >
            {t("断开账户")}
          </button>
        )}
        {extraActions}
      </div>
    </div>
  );
}
