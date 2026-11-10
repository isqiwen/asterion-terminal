import { defaultQuoteColumns } from "./QuoteColumns";
import { selectQuote, useSelectedQuote } from "./selected-quote";
import { productOverview } from "./product-overview";
import { QuotePane } from "./QuotePane";
import { WatchlistEditor } from "./WatchlistEditor";
import { WatchlistToggle } from "./WatchlistToggle";
import { useRef } from "react";
import type { QuoteRow } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";
import {
  contractName,
  productLabel,
  translate,
  useWorkspaceDraft,
  type TerminalContext,
} from "../contract";
import { QuoteTable, quoteChange } from "./QuoteTable";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
const venueNames: Record<string, string> = {
  SHFE: "上海期货交易所",
  DCE: "大连商品交易所",
  CZCE: "郑州商品交易所",
  CFFEX: "中国金融期货交易所",
  INE: "上海国际能源交易中心",
  GFEX: "广州期货交易所",
};
export const defaultSort: { column: string; direction: "ascending" | "descending" } = {
  column: "涨跌幅",
  direction: "descending",
};
// Groups of the whole board, in reading order; columns are filled top to bottom.
const boardGroups = [
  { title: "上海商品期货", venues: ["SHFE", "INE"] },
  { title: "郑州商品期货", venues: ["CZCE"] },
  { title: "大连商品期货", venues: ["DCE"] },
  { title: "金融期货", venues: ["CFFEX"] },
  { title: "广州商品期货", venues: ["GFEX"] },
];
export function MarketBoard({ market, context }: { market: MarketView; context: TerminalContext }) {
  const [columns] = useWorkspaceDraft("market-columns", defaultQuoteColumns);
  const visibleColumns = columns.filter(item => item.visible).map(item => item.column);
  const [scope] = useWorkspaceDraft("market-scope", "products");
  const [venue] = useWorkspaceDraft("market-venue", "");
  const [section] = useWorkspaceDraft("market-section", "期货全景");
  const saved = context.snapshot?.market_endpoint;
  const watchlist =
    section === "自选" && saved && "instruments" in saved ? saved.instruments : null;
  const selected = useSelectedQuote();
  const [search] = useWorkspaceDraft("quote-search", "");
  const [sort, setSort] = useWorkspaceDraft("market-sort", defaultSort);
  const board = useRef<HTMLDivElement>(null);
  const { rows: quotes, catalog } = market.board;
  const venueOf = (row: QuoteRow) => row.instrument!.exchange;
  const known = boardGroups.flatMap(group => group.venues);
  const others = [...new Set(quotes.map(venueOf))].filter(value => !known.includes(value)).sort();
  const filter = venue;
  const query = search.trim().toUpperCase();
  // Without a catalog there is no product identity; list subscribed months.
  const overview = !watchlist && scope === "products" && !query && !!catalog?.contracts.length;
  const candidates = watchlist
    ? quotes.filter(row =>
        watchlist.some(
          id => id.exchange === row.instrument!.exchange && id.symbol === row.instrument!.symbol,
        ),
      )
    : overview
      ? productOverview(market)
      : quotes;
  const rows = candidates.filter(
    row =>
      (!filter || venueOf(row) === filter) &&
      `${rowKey(row)} ${contractName(row.instrument!, catalog) ?? ""} ${productLabel(row.instrument!, catalog) ?? ""}`
        .toUpperCase()
        .includes(query),
  );
  const value = (row: QuoteRow) => {
    if (sort.column === "合约") return row.instrument!.symbol;
    if (sort.column === "涨跌幅") return quoteChange(row)?.percent ?? null;
    if (sort.column === "涨跌") return quoteChange(row)?.amount ?? null;
    if (sort.column === "1分钟涨速")
      return row.change1mPercent ? Number(row.change1mPercent.text) : null;
    if (sort.column === "成交量") return row.quote ? Number(row.quote.volume) : null;
    const field = sort.column === "最新价" ? row.quote?.last : row.quote?.openInterest;
    return field ? Number(field.text) : null;
  };
  rows.sort((a, b) => {
    const left = value(a),
      right = value(b);
    if (left == null) return right == null ? 0 : 1;
    if (right == null) return -1;
    const compared =
      typeof left === "string" && typeof right === "string"
        ? left.localeCompare(right)
        : Number(left) - Number(right);
    return compared * (sort.direction === "ascending" ? 1 : -1);
  });
  const groups = (
    filter
      ? [{ title: venueNames[filter] ?? filter, venues: [filter] }]
      : [...boardGroups, ...others.map(value => ({ title: value, venues: [value] }))]
  ).map(group => ({ ...group, rows: rows.filter(row => group.venues.includes(venueOf(row))) }));
  const orderedRows = groups.flatMap(group => group.rows);
  const active =
    rows.find(row => rowKey(row) === selected) ??
    orderedRows[0] ??
    quotes.find(row => rows.includes(row));
  const displayName = (row: QuoteRow) => {
    const label = overview ? productLabel(row.instrument!, catalog) : undefined;
    return label ? `${label}${t("主连")}` : contractName(row.instrument!, catalog);
  };
  const table = (groupRows: QuoteRow[]) => (
    <QuoteTable
      overviewStyle
      columns={visibleColumns}
      nameFor={displayName}
      market={market}
      rows={groupRows}
      selected={active ? rowKey(active) : undefined}
      onSelect={selectQuote}
      onOpen={id => {
        selectQuote(id);
        context.navigate("workspace.contract", { page: id });
      }}
      sort={sort}
      onSort={column =>
        setSort(previous => ({
          column,
          direction:
            previous.column === column && previous.direction === "descending"
              ? "ascending"
              : "descending",
        }))
      }
      onNavigate={(current, direction) => {
        const index = orderedRows.findIndex(row => rowKey(row) === current);
        const next = orderedRows[Math.max(0, Math.min(orderedRows.length - 1, index + direction))];
        if (next) {
          const id = rowKey(next);
          if (id !== selected) selectQuote(id);
          Array.from(board.current?.querySelectorAll<HTMLButtonElement>(".quote-select") ?? [])
            .find(button => button.dataset.quoteId === id)
            ?.focus();
        }
      }}
    />
  );
  return (
    <div className="futures-market-board" ref={board}>
      <div className="futures-market-board-list">
        {watchlist && <WatchlistEditor context={context} />}
        {!rows.length && (
          <p className="market-search-empty" role="status">
            {t(
              watchlist && !query
                ? "还没有自选合约。选中合约后，点图表标题旁的“加入自选”。"
                : quotes.length
                  ? "没有匹配的市场合约"
                  : "尚无订阅行情",
            )}
          </p>
        )}
        <div className="market-exchange-groups">
          {groups
            .filter(group => group.rows.length)
            .map(group => (
              <section key={group.title} aria-label={t(group.title)}>
                <h3>{t(group.title)}</h3>
                {table(group.rows)}
              </section>
            ))}
        </div>
      </div>
      {active && (
        <aside className="market-contract" aria-label={t("合约详情")}>
          <QuotePane
            context={context}
            market={market}
            row={active}
            name={displayName(active) ?? active.instrument!.symbol}
            pane="market-pane"
            initial="分时"
            actions={<WatchlistToggle context={context} market={market} row={active.instrument!} />}
          />
        </aside>
      )}
    </div>
  );
}
