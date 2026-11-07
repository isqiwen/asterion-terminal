import type { Snapshot, TradingConnection } from "../../bridge/client";
import { translate, useLocale } from "../../i18n";
import { useMarket } from "../market";
import { stalled, useTradingAccounts } from "../trading-accounts";
import { FeedState } from "../../generated/asterion/quotes_pb";
import { BrokerState } from "../../generated/asterion/trading_pb";

export function StatusIndicators({
  snapshot,
  openMarketSettings,
  openMarket,
  openTradingSettings,
  openTrading,
}: {
  snapshot: Snapshot | null;
  openMarketSettings: () => void;
  openMarket: () => void;
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
  const feed = market?.board.feed;
  // The link to Quotes first, then Quotes' own link to the broker.
  const [quotesTone, quotesState] = !market?.online
    ? ["bad", "中断"]
    : feed?.state === FeedState.CONNECTED
      ? ["good", ""]
      : feed?.state === FeedState.CONNECTING ||
          feed?.state === FeedState.LOGGING_IN ||
          feed?.state === FeedState.RECONNECTING
        ? ["", "连接柜台中"]
        : ["bad", "柜台未连接"];
  const label = (name: string, state: string) => `● ${name}${state && ` ${t(state)}`}`;
  return (
    <>
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
      {!(snapshot.quotes_endpoint && "host" in snapshot.quotes_endpoint) ? (
        <button className="status-item" onClick={openMarketSettings}>
          {label(t("行情"), "未设置")}
        </button>
      ) : !market ? (
        <button className="status-item" onClick={openMarket}>
          {label(t("行情"), "未连接")}
        </button>
      ) : (
        <span
          className={`status-item ${quotesTone}`}
          title={market.error || (feed?.ctpError ? `CTP ${feed.ctpError}` : undefined)}
        >
          {label(t("行情"), quotesState)}
        </span>
      )}
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
                : view.live?.brokerState === BrokerState.READY
                  ? ["good", view.live.authorizedMs !== undefined ? "已授权" : ""]
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
