import { useEffect, useState } from "react";
import type { Snapshot, TradingConnection } from "../../bridge/client";
import { warehouseData } from "../../bridge/warehouse-data";
import { translate, useLocale } from "../../i18n";
import { asDisplayError, errorSummary } from "../../i18n/errors";
import { useMarket } from "../market";
import { stalled, useTradingAccounts } from "../trading-accounts";
import { FeedState } from "../../generated/asterion/market_pb";
import { BrokerState } from "../../generated/asterion/trading_pb";

const label = (name: string, state: string) =>
  `● ${name}${state && ` ${translate("host", state)}`}`;

// Warehouse has no stream to watch, so its light comes from reading its status.
function WarehouseIndicator() {
  const [error, setError] = useState<string>();
  useEffect(() => {
    let active = true;
    let pending = false;
    const read = async () => {
      if (pending) return;
      pending = true;
      try {
        await warehouseData({ case: "getStatus", value: {} });
        if (active) setError("");
      } catch (failure) {
        if (active) setError(errorSummary(asDisplayError(failure)));
      } finally {
        pending = false;
      }
    };
    void read();
    const timer = window.setInterval(() => void read(), 5000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, []);
  const [tone, state] =
    error === undefined ? ["", "连接中"] : error ? ["bad", "中断"] : ["good", ""];
  return (
    <span className={`status-item ${tone}`} title={error || undefined}>
      {label(translate("host", "数据"), state)}
    </span>
  );
}

export function StatusIndicators({
  snapshot,
  openMarketSettings,
  openMarket,
  openWarehouseSettings,
  openTradingSettings,
  openTrading,
}: {
  snapshot: Snapshot | null;
  openMarketSettings: () => void;
  openMarket: () => void;
  openWarehouseSettings: () => void;
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
  // The link to Market first, then Market' own link to the broker.
  const [marketTone, marketState] = !market?.online
    ? ["bad", "中断"]
    : feed?.state === FeedState.CONNECTED
      ? ["good", ""]
      : feed?.state === FeedState.CONNECTING ||
          feed?.state === FeedState.LOGGING_IN ||
          feed?.state === FeedState.RECONNECTING
        ? ["", "连接柜台中"]
        : ["bad", "柜台未连接"];
  const warehouse = snapshot.warehouse_endpoint;
  // The sidebar's order; trading last, since there is one light per account.
  return (
    <>
      {!(snapshot.market_endpoint && "host" in snapshot.market_endpoint) ? (
        <button className="status-item" onClick={openMarketSettings}>
          {label(t("行情"), "未设置")}
        </button>
      ) : !market ? (
        <button className="status-item" onClick={openMarket}>
          {label(t("行情"), "未连接")}
        </button>
      ) : (
        <span
          className={`status-item ${marketTone}`}
          title={market.error || (feed?.ctpError ? `CTP ${feed.ctpError}` : undefined)}
        >
          {label(t("行情"), marketState)}
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
      {warehouse && "host" in warehouse ? (
        <WarehouseIndicator key={`${warehouse.host}:${warehouse.port}/${warehouse.fingerprint}`} />
      ) : (
        <button className="status-item" onClick={openWarehouseSettings}>
          {label(t("数据"), "未设置")}
        </button>
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
