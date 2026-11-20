import { useRef, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import type { State as EmergencyState } from "../../src/generated/asterion/emergency_pb";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { t, type Run } from "./live-shared";

const offsetLabels: Record<string, string> = {
  open: "开仓",
  close_today: "平今",
  close_yesterday: "平昨",
  close: "平仓",
};

export function EmergencyControls({
  account,
  state,
  control,
}: {
  account: TradingConnection;
  state: EmergencyState;
  control: Run;
}) {
  const revokingRef = useRef(false);
  const stoppingRef = useRef(false);
  const cancellingRef = useRef(new Set<string>());
  const [revoking, setRevoking] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [cancelling, setCancelling] = useState<readonly string[]>([]);

  const revoke = () => {
    if (revokingRef.current) return;
    revokingRef.current = true;
    setRevoking(true);
    void control(() => ({
      case: "emergencyControl",
      value: {
        action: {
          case: "revoke",
          value: {
            requestId: crypto.randomUUID(),
            accountId: state.accountId,
            policyRevision: state.policyRevision,
          },
        },
      },
    })).finally(() => {
      revokingRef.current = false;
      setRevoking(false);
    });
  };

  const stopStrategy = () => {
    if (stoppingRef.current || !state.activeStrategyId) return;
    stoppingRef.current = true;
    setStopping(true);
    const instanceId = state.activeStrategyId;
    void control(() => ({
      case: "emergencyControl",
      value: {
        action: {
          case: "stopStrategy",
          value: { instanceId },
        },
      },
    })).finally(() => {
      stoppingRef.current = false;
      setStopping(false);
    });
  };

  const cancelOrder = (orderId: string) => {
    if (cancellingRef.current.has(orderId)) return;
    cancellingRef.current.add(orderId);
    setCancelling(current => [...current, orderId]);
    void control(() => ({
      case: "emergencyControl",
      value: {
        action: {
          case: "cancelOrder",
          value: {
            requestId: crypto.randomUUID(),
            accountId: state.accountId,
            policyRevision: state.policyRevision,
            orderId,
          },
        },
      },
    })).finally(() => {
      cancellingRef.current.delete(orderId);
      setCancelling(current => current.filter(item => item !== orderId));
    });
  };

  return (
    <section aria-label={t("紧急安全控制")}>
      <div className="ctp-identity">
        <strong>{[account.instance, state.accountId].filter(Boolean).join(" · ")}</strong>
        <StatusBadge tone="attention">{t("紧急控制模式")}</StatusBadge>
        <StatusBadge tone={state.authorized ? "ok" : "neutral"}>
          {t(state.authorized ? "已允许下单" : "未允许下单")}
        </StatusBadge>
        {state.activeStrategyId && (
          <span>{t("策略实例 {id} 正在运行", { id: state.activeStrategyId })}</span>
        )}
        <div className="ctp-identity-actions">
          {state.authorized && (
            <button className="danger" disabled={revoking} onClick={revoke}>
              {t("停止下单")}
            </button>
          )}
          {state.activeStrategyId && (
            <button className="danger" disabled={stopping} onClick={stopStrategy}>
              {t("停止策略并撤单")}
            </button>
          )}
        </div>
      </div>
      <p className="alert" role="alert">
        {t("交易服务与 Terminal 的协议不一致，常规业务已禁用；仍可执行紧急制动。")}
      </p>
      {state.workingOrders.length > 0 ? (
        <table aria-label={t("CTP 委托记录")}>
          <thead>
            <tr>
              <th scope="col">{t("合约")}</th>
              <th scope="col">{t("方向 / 开平")}</th>
              <th scope="col">{t("委托手数")}</th>
              <th scope="col">{t("限价")}</th>
              <th scope="col">{t("操作")}</th>
            </tr>
          </thead>
          <tbody>
            {state.workingOrders.map(order => (
              <tr key={order.id}>
                <td>{`${order.exchange}.${order.symbol}`}</td>
                <td>
                  {`${order.side === "buy" ? t("买入") : t("卖出")} / ${t(offsetLabels[order.offset] ?? "平仓")}`}
                </td>
                <td>{order.lots}</td>
                <td>{order.price}</td>
                <td>
                  <button
                    className="danger-quiet"
                    disabled={cancelling.includes(order.id)}
                    onClick={() => cancelOrder(order.id)}
                  >
                    {t("撤单")}
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      ) : (
        <p className="subtle">{t("暂无委托")}</p>
      )}
    </section>
  );
}
