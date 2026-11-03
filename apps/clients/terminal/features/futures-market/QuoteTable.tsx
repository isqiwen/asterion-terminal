import { quoteColumns, type QuoteColumn } from "./QuoteColumns";
import "./quotes.css";
import { getLocale, translate } from "../contract";
import type { Subscription } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function quoteStatus(market: MarketView, row: Subscription, now = Date.now()) {
  const q = row.quote;
  if (!market.online || market.snapshot.phase !== "connected")
    return t(q ? "断线 · 旧报价" : "行情未连接");
  if (row.state === "error") return t("订阅失败");
  if (row.state !== "subscribed") return t("订阅中");
  if (!q) return t("等待首笔");
  const source = Number(q.sourceMs);
  if (!source) return t("时间未知");
  if (source > now + 5000) return t("来源时间超前");
  return t(now - source > 30000 || now - Number(q.receivedMs) > 30000 ? "行情未更新" : "实时");
}
// Display-only change relative to previous settlement; never a ledger calculation.
export function quoteChange(row: Subscription) {
  const q = row.quote;
  if (q?.last == null || q.previousSettlement == null) return null;
  const last = Number(q.last),
    base = Number(q.previousSettlement);
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
function speed(row: Subscription) {
  const value = row.change1mPercent == null ? NaN : Number(row.change1mPercent);
  return Number.isFinite(value) ? value : null;
}
export function QuoteTable({
  market,
  rows = market.snapshot.subscriptions,
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
  nameFor?: (row: Subscription) => string | undefined;
  sort?: { column: string; direction: "ascending" | "descending" };
  onSort?: (column: string) => void;
  market: MarketView;
  // The rows to show, in order; all of the market's rows by default.
  rows?: readonly Subscription[];
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
          {rows.map(row => {
            const id = rowKey(row);
            const { venue, symbol } = row.instrument!;
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
                key={id}
                data-selected={selected === id}
                onClick={
                  onSelect
                    ? event => {
                        onSelect(id);
                        event.currentTarget
                          .querySelector<HTMLButtonElement>(".quote-select")
                          ?.focus({ preventScroll: true });
                      }
                    : undefined
                }
                onDoubleClick={onOpen ? () => onOpen(id) : undefined}
              >
                <td>
                  {onSelect ? (
                    <button
                      className="quote-select"
                      data-quote-id={id}
                      aria-label={symbol}
                      title={name ? `${name} · ${symbol}` : symbol}
                      aria-pressed={selected === id}
                      onKeyDown={event => {
                        if (event.key === "Enter" && onOpen) {
                          event.preventDefault();
                          onOpen(id);
                          return;
                        }
                        if (!onNavigate || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
                        event.preventDefault();
                        onNavigate(id, event.key === "ArrowUp" ? -1 : 1);
                      }}
                    >
                      {name ?? symbol}
                    </button>
                  ) : (
                    <strong>{symbol}</strong>
                  )}
                  <small className="quote-venue">{venue}</small>
                  {onSelect && !overviewStyle && quoteStatus(market, row) !== t("实时") && (
                    <small className="quote-inline-status">
                      {quoteStatus(market, row)}
                      {row.errorCode !== 0 && ` · CTP ${row.errorCode}`}
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
                              q?.openInterest != null &&
                              Number(q.openInterest) >= 10000
                              ? `${(Number(q.openInterest) / 10000).toFixed(2)}${getLocale() === "zh-CN" ? "万" : " ×10k"}`
                              : (q?.openInterest ?? "—")
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
                      {q?.bid ?? "—"} / {q?.bidQuantity ?? "—"}
                    </td>
                    <td className="numeric">
                      {q?.ask ?? "—"} / {q?.askQuantity ?? "—"}
                    </td>
                    <>
                      <td className="numeric">{q?.volume ?? "—"}</td>
                      <td className="numeric">{q?.openInterest ?? "—"}</td>
                    </>
                    <td>
                      {q?.sourceMs ? (
                        <time
                          dateTime={new Date(Number(q.sourceMs)).toISOString()}
                          title={new Date(Number(q.sourceMs)).toLocaleString(getLocale(), {
                            timeZone: "Asia/Shanghai",
                            hour12: false,
                          })}
                        >
                          {new Date(Number(q.sourceMs)).toLocaleString(getLocale(), {
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
                    {row.errorCode !== 0 && (
                      <details>
                        <summary>{t("详情")}</summary>CTP {row.errorCode}
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
