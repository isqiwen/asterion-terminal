import "./quotes.css";
import { getLocale, translate } from "../contract";
import type { LiveMarket } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function marketPhase(market: LiveMarket | null | undefined) {
  if (!market) return t("行情服务未启动");
  if (!market.transport_online) return t("行情服务失联");
  const keys: Record<string, string> = {
    disconnected: "未登录",
    connecting: "连接中",
    logging_in: "登录中",
    connected: "已登录",
    reconnecting: "重连中",
    error: "连接失败",
    sdk_unavailable: "当前平台缺少 CTP 行情组件",
  };
  return t(keys[market.phase] ?? "连接失败");
}
export function quoteStatus(
  market: LiveMarket,
  row: LiveMarket["subscriptions"][number],
  now = Date.now(),
) {
  const q = row.quote;
  if (!market.transport_online || market.phase !== "connected")
    return t(q ? "断线 · 旧报价" : "行情未连接");
  if (row.state === "error") return t("订阅失败");
  if (row.state !== "subscribed") return t("订阅中");
  if (!q) return t("等待首笔");
  if (!q.source_ms) return t("时间未知");
  if (q.source_ms > now + 5000) return t("来源时间超前");
  return t(now - q.source_ms > 30000 || now - q.received_ms > 30000 ? "行情未更新" : "实时");
}
export function QuoteTable({ market, compact = false }: { market: LiveMarket; compact?: boolean }) {
  return (
    <div
      className="dashboard-table market-quote-table"
      tabIndex={0}
      role="region"
      aria-label={t("自选行情表")}
    >
      <table className="data-table">
        <thead>
          <tr>
            {[
              "合约",
              "最新价",
              "买一 / 量",
              "卖一 / 量",
              ...(compact ? [] : ["成交量", "持仓量"]),
              "更新时间（北京时间）",
              "状态",
            ].map(key => (
              <th key={key}>{t(key)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {market.subscriptions.map(row => {
            const q = row.quote;
            return (
              <tr key={`${row.venue}.${row.symbol}`}>
                <td>
                  <strong>{row.symbol}</strong>
                  <small className="quote-venue">{row.venue}</small>
                </td>
                <td className="numeric">{q?.last ?? "—"}</td>
                <td className="numeric">
                  {q?.bid ?? "—"} / {q?.bid_quantity ?? "—"}
                </td>
                <td className="numeric">
                  {q?.ask ?? "—"} / {q?.ask_quantity ?? "—"}
                </td>
                {!compact && (
                  <>
                    <td className="numeric">{q?.volume ?? "—"}</td>
                    <td className="numeric">{q?.open_interest ?? "—"}</td>
                  </>
                )}
                <td>
                  {q?.source_ms ? (
                    <time
                      dateTime={new Date(q.source_ms).toISOString()}
                      title={new Date(q.source_ms).toLocaleString(getLocale(), {
                        timeZone: "Asia/Shanghai",
                        hour12: false,
                      })}
                    >
                      {compact
                        ? new Date(q.source_ms).toLocaleTimeString(getLocale(), {
                            timeZone: "Asia/Shanghai",
                            hour12: false,
                          })
                        : new Date(q.source_ms).toLocaleString(getLocale(), {
                            timeZone: "Asia/Shanghai",
                            hour12: false,
                          })}
                    </time>
                  ) : (
                    "—"
                  )}
                </td>
                <td>
                  {quoteStatus(market, row)}
                  {row.error_code !== 0 && (
                    <details>
                      <summary>{t("详情")}</summary>CTP {row.error_code}
                    </details>
                  )}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
