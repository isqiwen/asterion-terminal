import { selectQuote, useSelectedQuote } from "./selected-quote";
import type { Subscription } from "../../src/generated/asterion/market_pb";
import { rowKey, useMarket, type MarketView } from "../../src/host/market";
import { useEffect, type ReactNode } from "react";
import { contractName, translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { quoteChange, quoteStatus } from "./QuoteTable";
import { ContractHistory } from "./ContractHistory";
import { QuoteChart } from "./QuoteChart";
import { WatchlistToggle } from "./WatchlistToggle";
import { MarketUnavailable } from "./MarketUnavailable";
import "./quote-workspaces.css";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
// A row of these lists: Market's row with its identity and display name.
type QuoteRow = Subscription & { venue: string; symbol: string; displayName?: string };
const quoteId = (row: QuoteRow) => rowKey(row);
const tone = (row: QuoteRow) => {
  const amount = quoteChange(row)?.amount;
  return amount == null || amount === 0 ? undefined : amount > 0 ? "up" : "down";
};
function cell(row: QuoteRow, name: string) {
  const q = row.quote,
    change = quoteChange(row);
  if (name === "代码") return row.symbol;
  if (name === "名称") return row.displayName ?? row.symbol;
  if (name === "现价") return q?.last ?? "—";
  if (name === "涨幅%")
    return change ? `${change.percent > 0 ? "+" : ""}${change.percent.toFixed(2)}%` : "—";
  if (name === "涨跌")
    return change ? `${change.amount > 0 ? "+" : ""}${Number(change.amount.toFixed(8))}` : "—";
  if (name === "昨收") return q?.previousClose ?? "—";
  if (name === "开盘") return q?.open ?? "—";
  if (name === "最高") return q?.high ?? "—";
  if (name === "最低") return q?.low ?? "—";
  if (name === "振幅%" && q?.high != null && q.low != null && Number(q.previousSettlement) > 0)
    return `${(((Number(q.high) - Number(q.low)) / Number(q.previousSettlement)) * 100).toFixed(2)}%`;
  return "—";
}
function QuoteHeader({
  row,
  market,
  actions,
}: {
  row: QuoteRow;
  market: MarketView;
  actions?: ReactNode;
}) {
  const status = quoteStatus(market, row);
  return (
    <header className="quote-pane-title">
      <strong title={row.displayName ? `${row.displayName} · ${row.symbol}` : row.symbol}>
        {row.displayName ?? row.symbol}
      </strong>
      <b data-tone={tone(row)}>{cell(row, "现价")}</b>
      <span data-tone={tone(row)}>
        {cell(row, "涨跌")} · {cell(row, "涨幅%")}
      </span>
      {actions}
      <small>{row.venue}</small>
      {status !== t("实时") && (
        <span className="quote-freshness" role="status">
          {status}
        </span>
      )}
    </header>
  );
}
// One contract in depth. The list on the left is the watchlist, or while
// searching, every contract that matches.
export function ContractWorkspace(context: TerminalContext) {
  const selected = useSelectedQuote();
  const [search, setSearch] = useWorkspaceDraft("quote-search", "");
  useEffect(() => {
    if (context.workspacePage) selectQuote(context.workspacePage);
  }, [context.workspacePage]);
  const market = useMarket();
  const saved = context.snapshot?.market_endpoint;
  const watchlist = saved && "instruments" in saved ? saved.instruments : [];
  const rows: QuoteRow[] = (market?.snapshot.subscriptions ?? []).map(row => ({
    ...row,
    venue: row.instrument!.venue,
    symbol: row.instrument!.symbol,
    displayName: contractName(row.instrument!, market?.snapshot.catalog),
  }));
  const query = search.trim().toLowerCase();
  const listed = query
    ? rows.filter(row => `${quoteId(row)} ${row.displayName ?? ""}`.toLowerCase().includes(query))
    : rows.filter(row => watchlist.some(id => id.venue === row.venue && id.symbol === row.symbol));
  const active =
    rows.find(row => quoteId(row) === (selected || context.workspacePage)) ?? listed[0];
  const [chart, setChart] = useWorkspaceDraft("contract-chart", "live");
  const historyAvailable = market?.online && market.snapshot.phase === "connected";
  const q = active?.quote;
  const bid = q?.bid != null ? Number(q.bidQuantity) : 0,
    ask = q?.ask != null ? Number(q.askQuantity) : 0;
  const total = bid + ask;
  if (!market) return <MarketUnavailable context={context} />;
  return (
    <section className="contract-workspace" aria-label={t("合约")}>
      <aside className="contract-watchlist" aria-label={t(query ? "搜索结果" : "自选")}>
        <header>{t(query ? "搜索结果" : "自选")}</header>
        <div className="contract-list-heading">
          <span>{t("名称")}</span>
          <span>{t("趋势")}</span>
          <span>{t("涨跌")}</span>
          <span>{t("涨速")}</span>
          <span>{t("成交量")}</span>
          <span>{t("现价")}</span>
        </div>
        {listed.map((row, index) => {
          const points = historyAvailable
            ? market.ticks
                .filter(p => p.venue === row.venue && p.symbol === row.symbol)
                .map(p => Number(p.price))
                .filter(Number.isFinite)
            : [];
          const low = Math.min(...points),
            range = Math.max(...points) - low;
          return (
            <button
              className="contract-list-row"
              key={quoteId(row)}
              aria-pressed={active && quoteId(row) === quoteId(active)}
              onClick={() => selectQuote(quoteId(row))}
              onKeyDown={event => {
                if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
                event.preventDefault();
                const nextIndex =
                  event.key === "Home"
                    ? 0
                    : event.key === "End"
                      ? listed.length - 1
                      : Math.max(
                          0,
                          Math.min(listed.length - 1, index + (event.key === "ArrowDown" ? 1 : -1)),
                        );
                selectQuote(quoteId(listed[nextIndex]));
                const next =
                  event.currentTarget.parentElement?.querySelectorAll<HTMLButtonElement>(
                    ".contract-list-row",
                  )[nextIndex];
                next?.focus({ preventScroll: true });
                next?.scrollIntoView({ block: "nearest" });
              }}
            >
              <span>
                <b title={row.displayName}>{row.displayName ?? row.symbol}</b>
                <small>{row.displayName ? row.symbol : row.venue}</small>
              </span>
              <svg viewBox="0 0 80 24" aria-label={t("最近报价走势")} data-tone={tone(row)}>
                {points.length > 1 && (
                  <polyline
                    fill="none"
                    stroke="currentColor"
                    points={points
                      .map(
                        (value, i) =>
                          `${(i / (points.length - 1)) * 80},${range ? 22 - ((value - low) / range) * 20 : 12}`,
                      )
                      .join(" ")}
                  />
                )}
              </svg>
              <span data-tone={tone(row)}>{cell(row, "涨跌")}</span>
              <span>—</span>
              <span>{row.quote?.volume ?? "—"}</span>
              <span data-tone={tone(row)}>
                {cell(row, "现价")}
                <small>{cell(row, "涨幅%")}</small>
              </span>
            </button>
          );
        })}
        {!listed.length && (
          <div className="quote-empty" role="status">
            {query ? (
              <>
                <p>{t("没有匹配的合约")}</p>
                <button onClick={() => setSearch("")}>{t("清除搜索")}</button>
              </>
            ) : (
              <p>{t("还没有自选合约。在市场页选中合约，点图表标题旁的“加入自选”。")}</p>
            )}
          </div>
        )}
      </aside>
      <div className="contract-center">
        <section className="contract-chart-panel">
          {active ? (
            <>
              <QuoteHeader row={active} market={market} />
              <nav className="quote-tabs" aria-label={t("图表类型")}>
                <button aria-pressed={chart === "live"} onClick={() => setChart("live")}>
                  {t("报价走势")}
                </button>
                <button aria-pressed={chart === "history"} onClick={() => setChart("history")}>
                  {t("历史 K 线")}
                </button>
              </nav>
              {chart === "live" ? (
                <QuoteChart market={market} row={active} />
              ) : (
                <ContractHistory
                  key={quoteId(active)}
                  venue={active.venue}
                  symbol={active.symbol}
                  context={context}
                />
              )}
            </>
          ) : (
            <p className="quote-empty">{t("选择合约查看图表")}</p>
          )}
        </section>
        <section className="contract-information" aria-label={t("相关合约")}>
          <header>{t("相关合约")}</header>
          {active &&
            rows
              .filter(
                row =>
                  row.symbol.replace(/\d/g, "").toLowerCase() ===
                  active.symbol.replace(/\d/g, "").toLowerCase(),
              )
              .map(row => (
                <button
                  key={quoteId(row)}
                  aria-pressed={quoteId(row) === quoteId(active)}
                  onClick={() => selectQuote(quoteId(row))}
                >
                  {row.symbol}
                </button>
              ))}
        </section>
      </div>
      <aside className="contract-right" aria-label={t("合约详情")}>
        {active ? (
          <>
            <section className="contract-summary">
              <QuoteHeader
                row={active}
                market={market}
                actions={<WatchlistToggle context={context} market={market} row={active} />}
              />
              <strong className="contract-last" data-tone={tone(active)}>
                {q?.last ?? "—"}
              </strong>
              <dl>
                {[
                  ["开盘", q?.open],
                  ["日增仓", q?.openInterestChange],
                  ["涨停", q?.upperLimit],
                  ["跌停", q?.lowerLimit],
                  ["均价", null],
                  ["最高", q?.high],
                  ["最低", q?.low],
                  ["昨结算", q?.previousSettlement],
                  ["持仓量", q?.openInterest],
                  ["成交量", q?.volume],
                  ["交易日", q?.tradingDay],
                ].map(([label, value]) => (
                  <div key={label}>
                    <dt>{t(String(label))}</dt>
                    <dd>{value ?? "—"}</dd>
                  </div>
                ))}
              </dl>
              <small>{quoteStatus(market, active)}</small>
            </section>
            <section className="contract-depth">
              <header>{t("买卖盘")}</header>
              <div
                className="depth-ratio"
                data-empty={total === 0}
                role="img"
                aria-label={t(total ? "买一卖一挂单量占比" : "暂无买卖盘数量")}
                title={t(total ? "买一卖一挂单量占比" : "暂无买卖盘数量")}
              >
                {total > 0 && <span style={{ width: `${(bid / total) * 100}%` }} />}
              </div>
              <div className="depth-level">
                <span>{q?.bid != null ? bid : "—"}</span>
                <b data-tone="down">{q?.bid ?? "—"}</b>
                <span>{t("买一")}</span>
                <span>{t("卖一")}</span>
                <b data-tone="up">{q?.ask ?? "—"}</b>
                <span>{q?.ask != null ? ask : "—"}</span>
              </div>
              {[2, 3, 4, 5].map(level => {
                const bid = q?.bidLevels[level - 2];
                const ask = q?.askLevels[level - 2];
                return (
                  <div className="depth-level" key={level}>
                    <span>{bid?.quantity ?? "—"}</span>
                    <b data-tone="down">{bid?.price ?? "—"}</b>
                    <span>
                      {t("买")}
                      {level}
                    </span>
                    <span>
                      {t("卖")}
                      {level}
                    </span>
                    <b data-tone="up">{ask?.price ?? "—"}</b>
                    <span>{ask?.quantity ?? "—"}</span>
                  </div>
                );
              })}
              {!q?.bidLevels.some(level => level.price != null) &&
                !q?.askLevels.some(level => level.price != null) && (
                  <p className="quote-empty">{t("当前报价未提供更多档位")}</p>
                )}
            </section>
            <section className="contract-ticks">
              <header>{t("成交明细")}</header>
              <div className="tick-heading">
                {["时间", "成交", "现手", "增仓", "开平"].map(name => (
                  <span key={name}>{t(name)}</span>
                ))}
              </div>
              <p className="quote-empty">{t("逐笔成交数据尚未接入")}</p>
            </section>
            <section className="contract-order">
              <header>{t("交易下单")}</header>
              <label>
                {t("合约")}
                <input readOnly value={active.symbol} />
              </label>
              <button className="primary" onClick={() => context.navigate("workspace.trading")}>
                {t("前往 CTP 交易")}
              </button>
              <p className="quote-empty">{t("此处只展示行情；委托在交易页的 CTP 账户中发送。")}</p>
            </section>
          </>
        ) : (
          <p className="quote-empty">{t("选择合约查看详情")}</p>
        )}
      </aside>
    </section>
  );
}
