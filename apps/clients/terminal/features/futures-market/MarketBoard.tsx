import { QuoteColumns, defaultQuoteColumns } from "./QuoteColumns";
import { selectQuote, useSelectedQuote } from "./selected-quote";
import { productOverview } from "./product-overview";
import { QuotePane } from "./QuotePane";
import { WatchlistToggle } from "./WatchlistToggle";
import { createPortal } from "react-dom";
import { useEffect, useRef, useState, type ReactNode } from "react";
import type { ContractQuote } from "../../src/generated/asterion/market_pb";
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
export function MarketBoard({
  market,
  context,
  toolbar,
}: {
  market: MarketView;
  context: TerminalContext;
  toolbar?: ReactNode;
}) {
  const [columns, setColumns] = useWorkspaceDraft("market-columns", defaultQuoteColumns);
  const [editingColumns, setEditingColumns] = useState(false);
  const visibleColumns = columns.filter(item => item.visible).map(item => item.column);
  const [scope, setScope] = useWorkspaceDraft("market-scope", "products");
  const [venue] = useWorkspaceDraft("market-venue", "");
  const [section] = useWorkspaceDraft("market-section", "期货全景");
  const saved = context.snapshot?.market_endpoint;
  const watchlist =
    section === "自选" && saved && "instruments" in saved ? saved.instruments : null;
  const selected = useSelectedQuote();
  const [search] = useWorkspaceDraft("quote-search", "");
  const [menuOpen] = useWorkspaceDraft("market-menu", false);
  const [sort, setSort] = useWorkspaceDraft<{
    column: string;
    direction: "ascending" | "descending";
  }>("market-sort", {
    column: "涨跌幅",
    direction: "descending",
  });
  const board = useRef<HTMLDivElement>(null);
  const { quotes, catalog, phase } = market.snapshot;
  const venueOf = (row: ContractQuote) => row.instrument!.venue;
  const venueOrder = ["SHFE", "CFFEX", "CZCE", "INE", "DCE", "GFEX"];
  const venues = [...new Set(quotes.map(venueOf))].sort((a, b) => {
    const rank = (value: string) =>
      venueOrder.includes(value) ? venueOrder.indexOf(value) : venueOrder.length;
    return rank(a) - rank(b) || a.localeCompare(b);
  });
  const filter = venue;
  const query = search.trim().toUpperCase();
  // Without a catalog there is no product identity; list subscribed months.
  const overview = !watchlist && scope === "products" && !query && !!catalog?.contracts.length;
  const candidates = watchlist
    ? quotes.filter(row =>
        watchlist.some(
          id => id.venue === row.instrument!.venue && id.symbol === row.instrument!.symbol,
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
  const value = (row: ContractQuote) => {
    if (sort.column === "合约") return row.instrument!.symbol;
    if (sort.column === "涨跌幅") return quoteChange(row)?.percent ?? null;
    if (sort.column === "涨跌") return quoteChange(row)?.amount ?? null;
    if (sort.column === "1分钟涨速")
      return row.change1mPercent == null ? null : Number(row.change1mPercent);
    const field =
      sort.column === "最新价"
        ? row.quote?.last
        : sort.column === "成交量"
          ? row.quote?.volume
          : row.quote?.openInterest;
    return field == null ? null : Number(field);
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
  // Board groups follow the reference layout: domestic commodities by
  // exchange city on the left, financial and other groups on the right.
  const groups = (
    filter
      ? [{ title: venueNames[filter] ?? filter, venues: [filter], column: 0, row: 0 }]
      : [
          { title: "上海商品期货", venues: ["SHFE", "INE"], column: 0, row: 0 },
          { title: "金融期货", venues: ["CFFEX"], column: 1, row: 0 },
          { title: "广州商品期货", venues: ["GFEX"], column: 1, row: 0 },
          { title: "郑州商品期货", venues: ["CZCE"], column: 0, row: 1 },
          { title: "大连商品期货", venues: ["DCE"], column: 0, row: 2 },
          ...venues
            .filter(value => !venueOrder.includes(value))
            .map(value => ({ title: value, venues: [value], column: 1, row: 2 })),
        ]
  ).map(group => ({ ...group, rows: rows.filter(row => group.venues.includes(venueOf(row))) }));
  const orderedRows = groups.flatMap(group => group.rows);
  const active =
    rows.find(row => rowKey(row) === selected) ??
    orderedRows[0] ??
    quotes.find(row => rows.includes(row));
  const displayName = (row: ContractQuote) => {
    const label = overview ? productLabel(row.instrument!, catalog) : undefined;
    return label ? `${label}${t("主连")}` : contractName(row.instrument!, catalog);
  };
  const [menuSlot, setMenuSlot] = useState<HTMLElement | null>(null);
  useEffect(() => setMenuSlot(document.getElementById("market-menu-slot")), [menuOpen]);
  const inlineToolbar = phase !== "connected" || !quotes.length;
  const controls = (
    <div className="market-board-tools">
      <nav className="market-exchanges" aria-label={t("合约范围")}>
        <button aria-pressed={scope === "products"} onClick={() => setScope("products")}>
          {t("品种概览")}
        </button>
        <button aria-pressed={scope === "months"} onClick={() => setScope("months")}>
          {t("全部月份")}
        </button>
      </nav>
      <p className="subtle">
        {t(query ? "搜索全部月份" : overview ? "每品种一行 · 按持仓量" : "全部实际月份合约")}
        {overview && ` · ${t("主连行显示同品种持仓量最大的实际月份合约，不是拼接的连续合约。")}`}
      </p>
      <button onClick={() => setEditingColumns(true)}>{t("行情表头设置")}</button>
      <span className="subtle">
        {rows.length} / {quotes.length} {t("合约")}
      </span>
    </div>
  );
  const table = (groupRows: ContractQuote[]) => (
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
  const rowCount = Math.max(...groups.map(group => group.row)) + 1;
  return (
    <div className="futures-market-board" ref={board}>
      {editingColumns && (
        <QuoteColumns
          value={columns}
          close={() => setEditingColumns(false)}
          apply={next => {
            setColumns(next);
            if (
              sort.column !== "合约" &&
              !next.some(item => item.column === sort.column && item.visible)
            )
              setSort({ column: "合约", direction: "ascending" });
          }}
        />
      )}
      {menuOpen &&
        menuSlot &&
        createPortal(
          <>
            {controls}
            {!inlineToolbar && toolbar}
          </>,
          menuSlot,
        )}
      <div className="futures-market-board-list">
        {inlineToolbar && toolbar}
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
        <div
          className="market-exchange-groups"
          data-single-column={!!filter || undefined}
          style={{ gridTemplateRows: `repeat(${rowCount}, auto)` }}
        >
          {Array.from({ length: rowCount }).flatMap((_, row) =>
            [0, 1].map(column => {
              const cell = groups.filter(
                group => group.row === row && group.column === column && group.rows.length,
              );
              if (!cell.length) return null;
              return (
                <div
                  className="market-exchange-cell"
                  key={`${row}:${column}`}
                  style={{ gridRow: row + 1, gridColumn: filter ? 1 : column + 1 }}
                >
                  {cell.map(group => (
                    <section key={group.title} aria-label={t(group.title)}>
                      <h3>{t(group.title)}</h3>
                      {table(group.rows)}
                    </section>
                  ))}
                </div>
              );
            }),
          )}
        </div>
      </div>
      {active && (
        <aside className="market-contract" aria-label={t("合约详情")}>
          <QuotePane
            context={context}
            market={market}
            row={active}
            name={displayName(active) ?? active.instrument!.symbol}
            pane="market-pane-upper"
            initial="分时"
            actions={<WatchlistToggle context={context} market={market} row={active.instrument!} />}
          />
          <QuotePane
            context={context}
            market={market}
            row={active}
            name={displayName(active) ?? active.instrument!.symbol}
            pane="market-pane-lower"
            initial="日K"
          />
        </aside>
      )}
    </div>
  );
}
