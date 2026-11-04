import type { Snapshot, TradingConnection } from "../../bridge/client";
import { translate, useLocale } from "../../i18n";
import { useMarket } from "../market";
import { stalled, useTradingAccounts } from "../trading-accounts";

// One indicator per service, in the sidebar's order: 行情 (Market), 研究 (Lab)
// and 交易 (each trading connection). The dot's colour is the state; a word
// follows only when something is not normal.
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
  if (!snapshot) return null;
  const lab = snapshot.lab;
  const connections = (snapshot.trading_connections ?? []).filter(
    (item): item is TradingConnection => !("error" in item),
  );
  const label = (name: string, state: string) => `● ${name}${state && ` ${t(state)}`}`;
  return (
    <>
      {!(snapshot.market_endpoint && "host" in snapshot.market_endpoint) ? (
        <button className="status-item" onClick={openSources}>
          {label(t("行情"), "未设置")}
        </button>
      ) : (
        <span className={`status-item ${market ? (market.online ? "good" : "bad") : ""}`}>
          {label(t("行情"), !market ? "未连接" : market.online ? "" : "中断")}
        </span>
      )}
      {lab &&
        (() => {
          // Lab restarts by itself while the Terminal runs; this only says where it is.
          const [tone, state] = {
            ready: ["good", ""],
            starting: ["", "启动中"],
            unresponsive: ["bad", "异常"],
            restarting: ["bad", "重启中"],
          }[lab.state];
          return (
            <span className={`status-item ${tone}`} title={lab.error || undefined}>
              {label(t("研究"), state)}
            </span>
          );
        })()}
      {!connections.length && (
        <button className="status-item" onClick={openTradingSettings}>
          {label(t("交易"), "未设置")}
        </button>
      )}
      {connections.map(connection => {
        const view = accounts[connection.id];
        const [tone, state] = !view
          ? ["", "连接中"]
          : view.failed
            ? ["bad", "中断"]
            : stalled(view.health)
              ? ["bad", "停滞"]
              : view.live?.recoveryRequired
                ? ["warning", "需恢复"]
                : view.live?.phase === "ready"
                  ? ["good", view.live.authorization ? "已授权" : ""]
                  : ["", "未就绪"];
        return (
          <button key={connection.id} className={`status-item ${tone}`} onClick={openTrading}>
            {label(`${t("交易")}·${connection.instance}`, state)}
          </button>
        );
      })}
    </>
  );
}
