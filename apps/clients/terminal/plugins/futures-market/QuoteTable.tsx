import { quoteColumns, type QuoteColumn } from "./QuoteColumns";
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
// Display-only change relative to previous settlement; never a ledger calculation.
export function quoteChange(row: LiveMarket["subscriptions"][number]) {
  const q = row.quote;
  if (q?.last == null || q.previous_settlement == null) return null;
  const last = Number(q.last),
    base = Number(q.previous_settlement);
  if (!Number.isFinite(last) || !Number.isFinite(base) || base <= 0) return null;
  return { amount: last - base, percent: ((last - base) / base) * 100 };
}
// Compact market-board headings; column identities stay unchanged.
const overviewHeadings: Record<string, string> = {
  合约: "名称",
  最新价: "现价",
  涨跌幅: "涨跌幅(结)",
  持仓量: "持仓",
};
function speed(row: LiveMarket["subscriptions"][number]) {
  const value = row.change_1m_percent == null ? NaN : Number(row.change_1m_percent);
  return Number.isFinite(value) ? value : null;
}
export function QuoteTable({
  market,
  overviewStyle = false,
  selected,
  onSelect,
  onNavigate,
  onOpen,
  sort,
  onSort,
  nameFor,
  columns = quoteColumns,
}: {
  columns?: readonly QuoteColumn[];
  nameFor?: (row: LiveMarket["subscriptions"][number]) => string | undefined;
  sort?: { column: string; direction: "ascending" | "descending" };
  onSort?: (column: string) => void;
  market: LiveMarket;
  overviewStyle?: boolean;
  selected?: string;
  onSelect?: (id: string) => void;
  onNavigate?: (id: string, direction: number) => void;
  onOpen?: (id: string) => void;
}) {
  return (
    <div className="market-quote-table" tabIndex={0} role="region" aria-label={t("自选行情表")}>
      <table className="data-table">
        <thead>
          <tr>
            {(onSelect
              ? ["合约", ...columns]
              : [
                  "合约",
                  "最新价",
                  "买一 / 量",
                  "卖一 / 量",
                  "成交量",
                  "持仓量",
                  "更新时间（北京时间）",
                  "状态",
                ]
            ).map(key => (
              <th
                key={key}
                className={
                  [
                    "最新价",
                    "涨跌幅",
                    "涨跌",
                    "1分钟涨速",
                    "持仓量",
                    "成交量",
                    "买一 / 量",
                    "卖一 / 量",
                  ].includes(key)
                    ? "numeric"
                    : key === "状态"
                      ? "quote-status"
                      : undefined
                }
                aria-sort={sort?.column === key ? sort.direction : undefined}
              >
                {onSort && key !== "状态" ? (
                  <button className="quote-sort" aria-label={t(key)} onClick={() => onSort(key)}>
                    <span className="quote-heading-label">
                      {t(overviewStyle ? (overviewHeadings[key] ?? key) : key)}
                    </span>
                    <span
                      className="quote-sort-direction"
                      aria-hidden="true"
                      style={{ visibility: sort?.column === key ? "visible" : "hidden" }}
                    >
                      {sort?.direction === "ascending" ? "↑" : "↓"}
                    </span>
                  </button>
                ) : (
                  <span className="quote-heading-label">{t(key)}</span>
                )}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {market.subscriptions.map(row => {
            const name = nameFor?.(row);
            const q = row.quote;
            const change = quoteChange(row);
            const tone = change
              ? change.amount > 0
                ? "up"
                : change.amount < 0
                  ? "down"
                  : "flat"
              : "flat";
            return (
              <tr
                title={quoteStatus(market, row)}
                key={`${row.venue}.${row.symbol}`}
                data-selected={selected === `${row.venue}.${row.symbol}`}
                onClick={
                  onSelect
                    ? event => {
                        onSelect(`${row.venue}.${row.symbol}`);
                        event.currentTarget
                          .querySelector<HTMLButtonElement>(".quote-select")
                          ?.focus({ preventScroll: true });
                      }
                    : undefined
                }
                onDoubleClick={onOpen ? () => onOpen(`${row.venue}.${row.symbol}`) : undefined}
              >
                <td>
                  {onSelect ? (
                    <button
                      className="quote-select"
                      data-quote-id={`${row.venue}.${row.symbol}`}
                      aria-label={row.symbol}
                      title={name ? `${name} · ${row.symbol}` : row.symbol}
                      aria-pressed={selected === `${row.venue}.${row.symbol}`}
                      onKeyDown={event => {
                        if (event.key === "Enter" && onOpen) {
                          event.preventDefault();
                          onOpen(`${row.venue}.${row.symbol}`);
                          return;
                        }
                        if (!onNavigate || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
                        event.preventDefault();
                        onNavigate(`${row.venue}.${row.symbol}`, event.key === "ArrowUp" ? -1 : 1);
                      }}
                    >
                      {name ?? row.symbol}
                    </button>
                  ) : (
                    <strong>{row.symbol}</strong>
                  )}
                  <small className="quote-venue">{row.venue}</small>
                  {onSelect && !overviewStyle && quoteStatus(market, row) !== t("实时") && (
                    <small className="quote-inline-status">
                      {quoteStatus(market, row)}
                      {row.error_code !== 0 && ` · CTP ${row.error_code}`}
                    </small>
                  )}
                </td>
                {onSelect ? (
                  columns.map(column => (
                    <td
                      key={column}
                      className={`numeric ${column === "持仓量" || column === "成交量" ? "quote-activity" : ""}`}
                      data-tone={
                        column === "持仓量" || column === "成交量"
                          ? undefined
                          : column === "1分钟涨速"
                            ? (speed(row) ?? 0) > 0
                              ? "up"
                              : (speed(row) ?? 0) < 0
                                ? "down"
                                : "flat"
                            : tone
                      }
                    >
                      {column === "1分钟涨速"
                        ? (speed(row)?.toFixed(2) ?? "—")
                        : column === "最新价"
                          ? (q?.last ?? "—")
                          : column === "持仓量"
                            ? overviewStyle &&
                              q?.open_interest != null &&
                              Number(q.open_interest) >= 10000
                              ? `${(Number(q.open_interest) / 10000).toFixed(2)}${getLocale() === "zh-CN" ? "万" : " ×10k"}`
                              : (q?.open_interest ?? "—")
                            : column === "成交量"
                              ? (q?.volume ?? "—")
                              : !change
                                ? "—"
                                : column === "涨跌幅"
                                  ? `${change.percent > 0 ? "+" : ""}${change.percent.toFixed(2)}%`
                                  : `${change.amount > 0 ? "+" : ""}${change.amount.toLocaleString(getLocale(), { maximumFractionDigits: 8 })}`}
                    </td>
                  ))
                ) : (
                  <>
                    <td className="numeric" data-tone={tone}>
                      {q?.last ?? "—"}
                    </td>
                    <td className="numeric">
                      {q?.bid ?? "—"} / {q?.bid_quantity ?? "—"}
                    </td>
                    <td className="numeric">
                      {q?.ask ?? "—"} / {q?.ask_quantity ?? "—"}
                    </td>
                    <>
                      <td className="numeric">{q?.volume ?? "—"}</td>
                      <td className="numeric">{q?.open_interest ?? "—"}</td>
                    </>
                    <td>
                      {q?.source_ms ? (
                        <time
                          dateTime={new Date(q.source_ms).toISOString()}
                          title={new Date(q.source_ms).toLocaleString(getLocale(), {
                            timeZone: "Asia/Shanghai",
                            hour12: false,
                          })}
                        >
                          {new Date(q.source_ms).toLocaleString(getLocale(), {
                            timeZone: "Asia/Shanghai",
                            hour12: false,
                          })}
                        </time>
                      ) : (
                        "—"
                      )}
                    </td>
                  </>
                )}
                {!onSelect && (
                  <td className="quote-status">
                    <span className="quote-status-label">{quoteStatus(market, row)}</span>
                    {row.error_code !== 0 && (
                      <details>
                        <summary>{t("详情")}</summary>CTP {row.error_code}
                      </details>
                    )}
                  </td>
                )}
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
