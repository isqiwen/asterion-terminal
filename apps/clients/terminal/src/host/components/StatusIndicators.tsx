import type { Snapshot } from "../../bridge/client";
import { translate, useLocale } from "../../i18n";
import { useMarket } from "../market";
import { stalled, useTradingAccounts } from "../trading-accounts";

// One indicator per service this Terminal uses: Market, Lab once it has been
// used, and each trading connection (or one saying there is none).
export function StatusIndicators({
  snapshot,
  openSources,
  openTradingSettings,
  openTrading,
}: {
  snapshot: Snapshot | null;
  openSources: () => void;
  openTradingSettings: () => void;
  openTrading: () => void;
}) {
  useLocale();
  const t = (key: string) => translate("host", key);
  const market = useMarket();
  const accounts = useTradingAccounts().views;
  const endpoint = snapshot?.market_endpoint && "host" in snapshot.market_endpoint;
  const lab = snapshot?.nodes
    .find(node => node.id === "local")
    ?.health?.services.find(service => service.kind === "lab");
  // Lab restarts by itself while the Terminal runs; this only says where it is.
  const labState = !lab
    ? null
    : lab.state !== "running"
      ? ["bad", "重启中"]
      : lab.health === "starting"
        ? ["", "启动中"]
        : ["ready", "awaiting_input"].includes(lab.health)
          ? ["good", ""]
          : ["bad", "异常"];
  return (
    <>
      {!snapshot ? null : !endpoint ? (
        <button className="status-item warning" onClick={openSources}>
          ● {t("Market 未设置")}
        </button>
      ) : (
        <span className={`status-item ${market ? (market.online ? "good" : "bad") : ""}`}>
          ● Market{market ? (market.online ? "" : ` ${t("连接中断")}`) : ` ${t("未连接")}`}
        </span>
      )}
      {lab && labState && (
        <span className={`status-item ${labState[0]}`} title={lab.error || undefined}>
          ● Lab{labState[1] && ` ${t(labState[1])}`}
        </span>
      )}
      {snapshot && !snapshot.trading_connections?.length && (
        <button className="status-item" onClick={openTradingSettings}>
          ● {t("交易未设置")}
        </button>
      )}
      {(snapshot?.trading_connections ?? []).map(connection => {
        if ("error" in connection) return null;
        const view = accounts[connection.id];
        const state = !view
          ? ["", "连接中"]
          : view.failed
            ? ["bad", "连接中断"]
            : stalled(view.health)
              ? ["bad", "处理停滞"]
              : view.live?.recoveryRequired
                ? ["warning", "需要恢复"]
                : view.live?.phase === "ready"
                  ? ["good", view.live.authorization ? "已授权" : ""]
                  : ["", "未就绪"];
        return (
          <button key={connection.id} className={`status-item ${state[0]}`} onClick={openTrading}>
            ● {connection.name}
            {state[1] && ` ${t(state[1])}`}
          </button>
        );
      })}
    </>
  );
}
