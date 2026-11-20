import type { Snapshot, TradingConnection } from "../../bridge/client";
import { translate, useLocale, type MessageValues } from "../../i18n";
import { ctpErrorSummary, diagnosticSummary, errorSummary } from "../../i18n/errors";
import { useMarket, useMarketOpenError } from "../market";
import { stalled, useTradingAccounts } from "../trading-accounts";
import { useWarehouse } from "../warehouse";
import { FeedState, Subscription } from "../../generated/asterion/market_pb";
import { BrokerState } from "../../generated/asterion/trading_pb";

const th = (key: string, values?: MessageValues) => translate("host", key, values);
const label = (name: string, state: string) => `● ${name}${state && ` ${th(state)}`}`;
const lowDiskBytes = 5n * 1024n ** 3n;
const formatGib = (bytes: bigint) => {
  const gib = Number(bytes) / 1024 ** 3;
  return `${gib >= 10 ? gib.toFixed(0) : gib.toFixed(1)} GB`;
};

export function StatusIndicators({
  snapshot,
  openMarketSettings,
  openMarket,
  openResearch,
  openWarehouseSettings,
  openDataset,
  openTradingSettings,
  openTrading,
}: {
  snapshot: Snapshot | null;
  openMarketSettings: () => void;
  openMarket: () => void;
  openResearch?: () => void;
  openWarehouseSettings: () => void;
  openDataset?: () => void;
  openTradingSettings: () => void;
  openTrading: (accountId?: string) => void;
}) {
  useLocale();
  const t = (key: string, values?: MessageValues) => translate("host", key, values);
  const market = useMarket();
  const marketOpenError = useMarketOpenError();
  const accounts = useTradingAccounts().views;
  const warehouseState = useWarehouse();
  const warehouse = snapshot?.warehouse_endpoint;
  const warehouseKey =
    warehouse && "host" in warehouse
      ? `${warehouse.host}:${warehouse.port}/${warehouse.fingerprint}`
      : "";
  if (!snapshot) return null;
  const lab = snapshot.lab;
  const connections = (snapshot.trading_connections ?? []).filter(
    (item): item is TradingConnection => !("error" in item),
  );
  const feed = market?.board.feed;
  // The link to Market first, then Market's own link to the broker.
  const [marketTone, marketState] = !market?.online
    ? ["danger", "中断"]
    : feed?.state === FeedState.CONNECTED
      ? ["ok", ""]
      : feed?.state === FeedState.CONNECTING
        ? ["", "连接柜台中"]
        : feed?.state === FeedState.LOGGING_IN
          ? ["", "登录柜台中"]
          : feed?.state === FeedState.RECONNECTING
            ? ["", "柜台重连中"]
            : ["danger", "柜台未连接"];
  const subscribedCount = (market?.board.rows ?? []).filter(
    row => row.subscription === Subscription.ACTIVE,
  ).length;
  const marketTelemetry =
    market?.online && feed?.state === FeedState.CONNECTED
      ? [
          subscribedCount > 0 && t("已订阅 {count} 个合约", { count: subscribedCount }),
          market.lastReceivedMs > 0 &&
            t("最新报价 {time}", {
              time: new Date(market.lastReceivedMs).toLocaleTimeString("sv-SE", {
                timeZone: "Asia/Shanghai",
                hour12: false,
              }),
            }),
        ]
          .filter(Boolean)
          .join(" · ") || undefined
      : undefined;
  const diskFree = warehouseState?.diskFree;
  const warehouseError = warehouseState?.error;
  const lowDisk = diskFree !== undefined && diskFree > 0n && diskFree < lowDiskBytes;
  const diskNote =
    diskFree !== undefined && diskFree > 0n ? `${t("磁盘可用")} ${formatGib(diskFree)}` : "";
  const [warehouseTone, warehouseStatus] =
    warehouseError === undefined
      ? ["", "连接中"]
      : warehouseError
        ? ["danger", "中断"]
        : lowDisk
          ? ["attention", ""]
          : ["ok", ""];
  const warehouseTitle = warehouseError
    ? errorSummary(warehouseError)
    : [warehouseState?.activity?.summary, diskNote].filter(Boolean).join(" · ") || undefined;
  const warehouseText =
    lowDisk && !warehouseStatus
      ? `${label(t("数据"), "")} · ${diskNote}`
      : label(t("数据"), warehouseStatus);
  const activeTask = warehouseKey && !warehouseError ? (warehouseState?.activity ?? null) : null;
  const determinate = Boolean(activeTask && activeTask.total > 0n);
  const percent = determinate
    ? Math.min(100, Math.floor((Number(activeTask!.done) * 100) / Number(activeTask!.total)))
    : 0;
  const rateSuffix = activeTask && activeTask.ratePerMin > 0 ? ` · ${activeTask.ratePerMin}/m` : "";
  const queueSuffix = activeTask && activeTask.queued > 0 ? ` +${activeTask.queued}` : "";
  const taskMetric = activeTask
    ? determinate
      ? `${percent}% · ${activeTask.done}/${activeTask.total}${rateSuffix}${queueSuffix}`
      : activeTask.requests > 0n
        ? `${t("{count} 次请求", { count: activeTask.requests.toString() })}${rateSuffix}${queueSuffix}`
        : `${t("等待")}${queueSuffix}`
    : "";
  // The sidebar's order; trading last, since there is one light per account.
  return (
    <>
      {!(snapshot.market_endpoint && "host" in snapshot.market_endpoint) ? (
        <button type="button" className="status-item" onClick={openMarketSettings}>
          {label(t("行情"), "未设置")}
        </button>
      ) : !market ? (
        <button
          type="button"
          className="status-item"
          data-tone={marketOpenError ? "danger" : undefined}
          title={marketOpenError ? errorSummary(marketOpenError) : undefined}
          onClick={openMarket}
        >
          {label(t("行情"), marketOpenError ? "中断" : "连接中")}
        </button>
      ) : (
        <button
          type="button"
          className="status-item"
          data-tone={marketTone || undefined}
          title={
            market.error ||
            (feed?.error ? diagnosticSummary(feed.error) || feed.error : undefined) ||
            (feed?.ctpError ? ctpErrorSummary(feed.ctpError) : undefined) ||
            marketTelemetry
          }
          onClick={openMarket}
        >
          {label(t("行情"), marketState)}
        </button>
      )}
      {lab &&
        (() => {
          // Lab restarts by itself while the Terminal runs; this only says where it is.
          const [tone, state] = {
            ready: ["ok", ""],
            starting: ["", "启动中"],
            unresponsive: ["danger", "异常"],
            restarting: ["danger", "重启中"],
          }[lab.state];
          return openResearch ? (
            <button
              type="button"
              className="status-item"
              data-tone={tone || undefined}
              title={lab.error || undefined}
              onClick={openResearch}
            >
              {label(t("研究"), state)}
            </button>
          ) : (
            <span
              className="status-item"
              data-tone={tone || undefined}
              title={lab.error || undefined}
            >
              {label(t("研究"), state)}
            </span>
          );
        })()}
      {!warehouseKey ? (
        <button type="button" className="status-item" onClick={openWarehouseSettings}>
          {label(t("数据"), "未设置")}
        </button>
      ) : openDataset ? (
        <button
          type="button"
          className="status-item"
          data-tone={warehouseTone || undefined}
          title={warehouseTitle}
          onClick={openDataset}
        >
          {warehouseText}
        </button>
      ) : (
        <span className="status-item" data-tone={warehouseTone || undefined} title={warehouseTitle}>
          {warehouseText}
        </span>
      )}
      {!connections.length && (
        <button type="button" className="status-item" onClick={openTradingSettings}>
          {label(t("交易"), "未设置")}
        </button>
      )}
      {connections.map(connection => {
        const view = accounts[connection.id];
        const [tone, state] = !view
          ? ["", "连接中"]
          : view.failed
            ? ["danger", "中断"]
            : stalled(view.health)
              ? ["danger", "停滞"]
              : view.live?.recoveryRequired
                ? ["attention", "需恢复"]
                : view.live?.brokerState === BrokerState.READY
                  ? ["ok", view.live.authorizedMs !== undefined ? "已允许下单" : ""]
                  : view.live?.authorizedMs !== undefined
                    ? ["attention", "未就绪"]
                    : ["", "未就绪"];
        const title = !view
          ? undefined
          : view.failed
            ? errorSummary(view.failed)
            : stalled(view.health)
              ? t("交易服务有一部分处理超过 30 秒没有进展，请查看交易主机的日志。")
              : view.live?.brokerState === BrokerState.FAILED && view.live.ctpError
                ? ctpErrorSummary(view.live.ctpError)
                : view.live?.riskBreaches.length
                  ? t("账户触发硬风控，已暂停新增开仓。")
                  : view.live?.authorizedMs !== undefined &&
                      view.live.brokerState !== BrokerState.READY
                    ? `${t("已允许下单")} · ${t("未就绪")}`
                    : [view.account?.broker?.brokerId, view.account?.broker?.userId]
                        .filter(Boolean)
                        .join(" · ") || undefined;
        return (
          <button
            key={connection.id}
            type="button"
            className="status-item"
            data-tone={tone || undefined}
            title={title}
            onClick={() => openTrading(connection.id)}
          >
            {label(`${t("交易")}·${connection.instance}`, state)}
          </button>
        );
      })}
      {activeTask && (
        <>
          <span className="panel-spacer" />
          {openDataset ? (
            <button
              type="button"
              className="status-task"
              title={warehouseTitle}
              onClick={openDataset}
            >
              <span className="status-task-label">{activeTask.heading}</span>
              <progress
                className="status-progress"
                aria-label={activeTask.heading}
                value={determinate ? Number(activeTask.done) : undefined}
                max={determinate ? Number(activeTask.total) : undefined}
              />
              <span className="status-metric">{taskMetric}</span>
            </button>
          ) : (
            <span className="status-task" title={warehouseTitle}>
              <span className="status-task-label">{activeTask.heading}</span>
              <progress
                className="status-progress"
                aria-label={activeTask.heading}
                value={determinate ? Number(activeTask.done) : undefined}
                max={determinate ? Number(activeTask.total) : undefined}
              />
              <span className="status-metric">{taskMetric}</span>
            </span>
          )}
        </>
      )}
    </>
  );
}
