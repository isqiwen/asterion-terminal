import { recordQuoteVisit, useRecentQuotes } from "./recent-quotes";
import { contractName } from "./contract-name";
import type { LiveMarket } from "../../src/bridge/client";
import { useEffect, useState, type ReactNode } from "react";
import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { quoteChange, quoteStatus } from "./QuoteTable";
import { ContractHistory } from "./ContractHistory";
import { QuoteChart, quoteId, type QuoteRow } from "./QuoteChart";
import { WatchlistToggle } from "./WatchlistToggle";
import "./quote-workspaces.css";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
const fields = [
  "代码",
  "名称",
  "涨幅%",
  "现价",
  "涨跌",
  "主力净额",
  "主力净量",
  "量比",
  "1分钟涨速",
  "总市值",
  "流通市值",
  "净利润",
  "成交额",
  "换手%",
  "振幅%",
  "市盈(动)",
  "最高",
  "最低",
  "开盘",
  "昨收",
  "外盘",
  "内盘",
];
const sortableFields = new Set([
  "代码",
  "名称",
  "涨幅%",
  "现价",
  "涨跌",
  "最高",
  "最低",
  "开盘",
  "昨收",
  "振幅%",
]);
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
  if (name === "昨收") return q?.previous_close ?? "—";
  if (name === "开盘") return q?.open ?? "—";
  if (name === "最高") return q?.high ?? "—";
  if (name === "最低") return q?.low ?? "—";
  if (name === "振幅%" && q?.high != null && q.low != null && Number(q.previous_settlement) > 0)
    return `${(((Number(q.high) - Number(q.low)) / Number(q.previous_settlement)) * 100).toFixed(2)}%`;
  return "—";
}
function QuoteHeader({
  row,
  market,
  actions,
}: {
  row: QuoteRow;
  market: LiveMarket;
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
function useQuotes(context: TerminalContext, watchlistOnly = false) {
  const [selected = ""] = useRecentQuotes();
  const [search, setSearch] = useWorkspaceDraft("quote-search", "");
  useEffect(() => {
    if (context.workspacePage) {
      recordQuoteVisit(context.workspacePage);
    }
  }, [context.workspacePage]);
  const market = context.snapshot?.market;
  const rows = (market?.subscriptions ?? [])
    .filter(
      row =>
        !watchlistOnly ||
        market?.watchlist?.some(id => id.venue === row.venue && id.symbol === row.symbol),
    )
    .map(row => ({
      ...row,
      displayName: contractName(row, context.snapshot?.history_contracts, market?.catalog),
    }))
    .filter(row =>
      `${quoteId(row)} ${row.displayName ?? ""}`
        .toLowerCase()
        .includes(search.trim().toLowerCase()),
    );
  const active = rows.find(row => quoteId(row) === (selected || context.workspacePage)) ?? rows[0];
  return {
    market,
    rows,
    active,
    filtering: search.trim().length > 0,
    clearSearch: () => setSearch(""),
    select: (id: string) => {
      recordQuoteVisit(id);
    },
  };
}
export function WatchlistWorkspace(context: TerminalContext) {
  const [recent, setRecent] = useState(false);
  const {
    market,
    rows,
    active: selectedRow,
    select,
    filtering,
    clearSearch,
  } = useQuotes(context, !recent);
  const visited = useRecentQuotes();
  const [recentOrder, setRecentOrder] = useState<readonly string[]>([]);
  const [sort, setSort] = useWorkspaceDraft(recent ? "recent-quotes-sort" : "watchlist-sort", {
    name: "",
    descending: true,
  });
  const pick = (row: QuoteRow) => {
    select(quoteId(row));
  };
  const visible = (
    recent ? recentOrder.flatMap(id => rows.filter(row => quoteId(row) === id)) : [...rows]
  ).sort((a, b) => {
    if (!sort.name) return 0;
    const left = cell(a, sort.name),
      right = cell(b, sort.name);
    if (left === "—") return right === "—" ? 0 : 1;
    if (right === "—") return -1;
    const order = ["代码", "名称"].includes(sort.name)
      ? left.localeCompare(right)
      : parseFloat(left) - parseFloat(right);
    return order * (sort.descending ? -1 : 1);
  });
  const active =
    visible.find(row => selectedRow && quoteId(row) === quoteId(selectedRow)) ?? visible[0];
  return (
    <section className="watchlist-workspace" aria-label={t("自选")}>
      <div className="watchlist-sheet">
        <nav className="quote-tabs" aria-label={t("自选视图")}>
          {[false, true].map(value => (
            <button
              key={String(value)}
              aria-pressed={recent === value}
              onClick={() => {
                setRecentOrder(visited);
                setRecent(value);
              }}
            >
              {t(value ? "最近浏览" : "自选")}
            </button>
          ))}
          <button
            className="quote-settings"
            onClick={() => context.navigate("workspace.market", { marketMode: "live" })}
          >
            {t("去市场添加自选")}
          </button>
        </nav>
        <div className="quote-group-strip">{t("期货自选")}</div>
        <div className="watchlist-table-scroll">
          <table className="watchlist-table">
            <thead>
              <tr>
                <th>#</th>
                {fields.map(name => (
                  <th
                    key={name}
                    aria-sort={
                      sort.name === name
                        ? sort.descending
                          ? "descending"
                          : "ascending"
                        : undefined
                    }
                  >
                    <button
                      disabled={!sortableFields.has(name)}
                      onClick={() =>
                        setSort({ name, descending: sort.name === name ? !sort.descending : true })
                      }
                    >
                      <span>{t(name)}</span>
                    </button>
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {visible.map((row, index) => (
                <tr
                  key={quoteId(row)}
                  data-selected={active && quoteId(active) === quoteId(row)}
                  onClick={event => {
                    pick(row);
                    event.currentTarget
                      .querySelector<HTMLButtonElement>("td > button")
                      ?.focus({ preventScroll: true });
                  }}
                  onDoubleClick={() =>
                    context.navigate("workspace.contract", { page: quoteId(row) })
                  }
                >
                  <td>{index + 1}</td>
                  {fields.map((name, column) => (
                    <td
                      key={name}
                      data-tone={
                        column < 2 ? "symbol" : cell(row, name) === "—" ? undefined : tone(row)
                      }
                    >
                      {column === 0 ? (
                        <button
                          aria-pressed={active && quoteId(active) === quoteId(row)}
                          onKeyDown={event => {
                            if (event.key === "Enter") {
                              event.preventDefault();
                              context.navigate("workspace.contract", { page: quoteId(row) });
                              return;
                            }
                            if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
                              event.preventDefault();
                              const next =
                                visible[
                                  event.key === "Home"
                                    ? 0
                                    : event.key === "End"
                                      ? visible.length - 1
                                      : Math.max(
                                          0,
                                          Math.min(
                                            visible.length - 1,
                                            index + (event.key === "ArrowDown" ? 1 : -1),
                                          ),
                                        )
                                ];
                              if (next) {
                                pick(next);
                                const buttons = event.currentTarget
                                  .closest("tbody")
                                  ?.querySelectorAll<HTMLButtonElement>("td > button");
                                buttons?.[visible.indexOf(next)]?.focus();
                              }
                            }
                          }}
                        >
                          {cell(row, name)}
                        </button>
                      ) : (
                        <span title={cell(row, name)}>{cell(row, name)}</span>
                      )}
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        {!visible.length && (
          <div className="quote-empty" role="status">
            <p>
              {t(
                filtering
                  ? "没有匹配的自选合约"
                  : recent
                    ? "尚无最近浏览合约"
                    : market?.phase !== "connected"
                      ? "实时行情未连接，自选合约没有报价。"
                      : "自选为空。在市场页选中合约，点图表标题旁的“加入自选”。",
              )}
            </p>
            {filtering && <button onClick={clearSearch}>{t("清除搜索")}</button>}
            {!filtering && !recent && (
              <button
                className="primary"
                onClick={() => context.navigate("workspace.market", { marketMode: "live" })}
              >
                {t(market?.phase !== "connected" ? "连接行情" : "去市场添加自选")}
              </button>
            )}
          </div>
        )}
        <div className="quote-sheet-note">
          {t("未提供的字段显示 —；名称取自已加载目录，缺失时显示代码。双击合约查看详情。")}
        </div>
      </div>
      <aside className="watchlist-charts" aria-label={t("自选合约图表")}>
        {active && market ? (
          <>
            <div className="watchlist-chart-pane">
              <QuoteHeader
                row={active}
                market={market}
                actions={<WatchlistToggle context={context} market={market} row={active} />}
              />
              <ContractHistory
                key={`upper:${quoteId(active)}`}
                venue={active.venue}
                symbol={active.symbol}
                context={context}
              />
            </div>
            <div className="watchlist-chart-pane">
              <QuoteHeader row={active} market={market} />
              <ContractHistory
                key={`lower:${quoteId(active)}`}
                venue={active.venue}
                symbol={active.symbol}
                context={context}
                preferLongest
              />
            </div>
          </>
        ) : (
          <p className="quote-empty">{t("选择合约查看图表")}</p>
        )}
      </aside>
    </section>
  );
}
export function ContractWorkspace(context: TerminalContext) {
  const { market, rows, active, select, filtering, clearSearch } = useQuotes(context);
  const [chart, setChart] = useWorkspaceDraft("contract-chart", "live");
  const [info, setInfo] = useState("资讯");
  const historyAvailable =
    market?.transport_online && market.phase === "connected" && market.history?.available;
  const q = active?.quote;
  const bid = q?.bid != null ? q.bid_quantity : 0,
    ask = q?.ask != null ? q.ask_quantity : 0;
  const total = bid + ask;
  return (
    <section className="contract-workspace" aria-label={t("合约")}>
      <aside className="contract-watchlist" aria-label={t("自选列表")}>
        <header>
          {t("自选列表")}
          <button onClick={() => context.navigate("workspace.watchlist")}>{t("全部自选")}</button>
        </header>
        <div className="contract-list-heading">
          <span>{t("名称")}</span>
          <span>{t("趋势")}</span>
          <span>{t("涨跌")}</span>
          <span>{t("涨速")}</span>
          <span>{t("成交量")}</span>
          <span>{t("现价")}</span>
        </div>
        {rows.map((row, index) => {
          const points = historyAvailable
            ? (market.history?.points
                .filter(p => p.venue === row.venue && p.symbol === row.symbol)
                .map(p => Number(p.price))
                .filter(Number.isFinite) ?? [])
            : [];
          const low = Math.min(...points),
            range = Math.max(...points) - low;
          return (
            <button
              className="contract-list-row"
              key={quoteId(row)}
              aria-pressed={active && quoteId(row) === quoteId(active)}
              onClick={() => select(quoteId(row))}
              onKeyDown={event => {
                if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
                event.preventDefault();
                const nextIndex =
                  event.key === "Home"
                    ? 0
                    : event.key === "End"
                      ? rows.length - 1
                      : Math.max(
                          0,
                          Math.min(rows.length - 1, index + (event.key === "ArrowDown" ? 1 : -1)),
                        );
                select(quoteId(rows[nextIndex]));
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
        {!rows.length &&
          (filtering ? (
            <div className="quote-empty" role="status">
              <p>{t("没有匹配的自选合约")}</p>
              <button onClick={clearSearch}>{t("清除搜索")}</button>
            </div>
          ) : (
            <button onClick={() => context.navigate("workspace.market")}>{t("连接行情")}</button>
          ))}
      </aside>
      <div className="contract-center">
        <section className="contract-chart-panel">
          <nav className="quote-tabs">
            <button aria-pressed>{t("图表")}</button>
            <button disabled>{t("T形报价表")}</button>
            <button disabled>F10</button>
          </nav>
          {active && market ? (
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
        <section className="contract-information">
          <nav className="quote-tabs" aria-label={t("合约信息")}>
            {["资讯", "相关合约", "外盘合约", "关联品种", "社区"].map(name => (
              <button key={name} aria-pressed={info === name} onClick={() => setInfo(name)}>
                {t(name)}
              </button>
            ))}
          </nav>
          {info === "相关合约" && active ? (
            rows
              .filter(
                row =>
                  row.symbol.replace(/\d/g, "").toLowerCase() ===
                  active.symbol.replace(/\d/g, "").toLowerCase(),
              )
              .map(row => (
                <button key={quoteId(row)} onClick={() => select(quoteId(row))}>
                  {row.symbol}
                </button>
              ))
          ) : (
            <p className="quote-empty">{t("该信息源尚未接入")}</p>
          )}
        </section>
      </div>
      <aside className="contract-right" aria-label={t("合约详情")}>
        {active && market ? (
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
                  ["日增仓", q?.open_interest_change],
                  ["涨停", q?.upper_limit],
                  ["跌停", q?.lower_limit],
                  ["均价", null],
                  ["最高", q?.high],
                  ["最低", q?.low],
                  ["昨结算", q?.previous_settlement],
                  ["持仓量", q?.open_interest],
                  ["成交量", q?.volume],
                  ["交易日", q?.trading_day],
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
                const bid = q?.bid_levels?.[level - 2];
                const ask = q?.ask_levels?.[level - 2];
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
              {!q?.bid_levels?.some(level => level.price !== null) &&
                !q?.ask_levels?.some(level => level.price !== null) && (
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
