import { defaultQuoteColumns } from "./QuoteColumns";
import { selectQuote, useSelectedQuote } from "./selected-quote";
import { productOverview } from "./product-overview";
import { QuotePane } from "./QuotePane";
import { ContractQuote, sameProduct } from "./ContractQuote";
import { WatchlistEditor } from "./WatchlistEditor";
import { WatchlistToggle } from "./WatchlistToggle";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { boardColumns } from "./board-columns";
import type { QuoteRow } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { translate } from "../../src/i18n";
import { contractName, productLabel } from "../../src/ui/contract-names";
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
// The narrowest column a quote table fits in without scrolling sideways.
const columnWidth = 460;
export function MarketBoard({ market, context }: { market: MarketView; context: TerminalContext }) {
  const list = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(0);
  useEffect(() => {
    const observer = new ResizeObserver(([entry]) => setWidth(entry!.contentRect.width));
    observer.observe(list.current!);
    return () => observer.disconnect();
  }, []);
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
  const filter = venue;
  const query = search.trim().toUpperCase();
  // Without a catalog there is no product identity; list subscribed months.
  const dominantOnly = section === "主力合约" && !!catalog?.contracts.length;
  const overview =
    !watchlist && !dominantOnly && scope === "products" && !query && !!catalog?.contracts.length;
  const { rows, shown, orderedRows } = useMemo(() => {
    const venueOf = (row: QuoteRow) => row.instrument!.exchange;
    const known = new Set(boardGroups.flatMap(group => group.venues));
    const others = [...new Set(quotes.map(venueOf))].filter(value => !known.has(value)).sort();
    const watchlistKeys = watchlist
      ? new Set(watchlist.map(id => `${id.exchange}.${id.symbol.toUpperCase()}`))
      : null;
    const candidates = watchlistKeys
      ? quotes.filter(row =>
          watchlistKeys.has(`${row.instrument!.exchange}.${row.instrument!.symbol.toUpperCase()}`),
        )
      : dominantOnly || overview
        ? productOverview(market)
        : quotes;
    const filtered = candidates.filter(
      row =>
        (!filter || venueOf(row) === filter) &&
        (!query ||
          `${rowKey(row)} ${contractName(row.instrument!, catalog) ?? ""} ${productLabel(row.instrument!, catalog) ?? ""}`
            .toUpperCase()
            .includes(query)),
    );
    const numeric = (value: number | undefined | null) =>
      value != null && Number.isFinite(value) ? value : null;
    const value = (row: QuoteRow) => {
      if (sort.column === "合约") return row.instrument!.symbol;
      if (sort.column === "涨跌幅") return numeric(quoteChange(row)?.percent);
      if (sort.column === "涨跌") return numeric(quoteChange(row)?.amount);
      if (sort.column === "1分钟涨速")
        return row.change1mPercent ? numeric(Number(row.change1mPercent.text)) : null;
      if (sort.column === "成交量") return row.quote ? numeric(Number(row.quote.volume)) : null;
      const field = sort.column === "最新价" ? row.quote?.last : row.quote?.openInterest;
      return field ? numeric(Number(field.text)) : null;
    };
    const direction = sort.direction === "ascending" ? 1 : -1;
    const keyed = filtered.map(row => ({ row, key: value(row) }));
    keyed.sort((a, b) => {
      const left = a.key,
        right = b.key;
      if (left == null) return right == null ? 0 : 1;
      if (right == null) return -1;
      const compared =
        typeof left === "string" && typeof right === "string"
          ? left < right
            ? -1
            : left > right
              ? 1
              : 0
          : Number(left) - Number(right);
      return compared * direction;
    });
    const sorted = keyed.map(item => item.row);
    const groups = (
      filter
        ? [
            {
              title: filter,
              label: venueNames[filter] ? t(venueNames[filter]) : filter,
              venues: [filter],
            },
          ]
        : [
            ...boardGroups.map(group => ({ ...group, label: t(group.title) })),
            ...others.map(value => ({ title: value, label: value, venues: [value] })),
          ]
    ).map(group => ({ ...group, rows: sorted.filter(row => group.venues.includes(venueOf(row))) }));
    const visible = groups.filter(group => group.rows.length);
    return {
      rows: sorted,
      shown: visible,
      orderedRows: visible.flatMap(group => group.rows),
    };
  }, [quotes, catalog, watchlist, dominantOnly, overview, market, filter, query, sort]);
  const firstRowId = orderedRows[0] ? rowKey(orderedRows[0]) : "";
  useEffect(() => {
    if (!selected && firstRowId) selectQuote(firstRowId);
  }, [selected, firstRowId]);
  // Another month of a listed product, chosen beside the chart, stays chosen.
  const chosen = quotes.find(row => rowKey(row) === selected);
  const active =
    rows.find(row => rowKey(row) === selected) ??
    (chosen && rows.some(row => sameProduct(row, chosen)) ? chosen : undefined) ??
    orderedRows[0];
  const listed =
    active && (rows.includes(active) ? active : rows.find(row => sameProduct(row, active)));
  const [expanded, setExpanded] = useWorkspaceDraft("market-expanded", false);
  const displayName = useCallback(
    (row: QuoteRow) => {
      const label = overview ? productLabel(row.instrument!, catalog) : undefined;
      return label ? `${label}${t("主连")}` : contractName(row.instrument!, catalog);
    },
    [overview, catalog],
  );
  const handleOpen = useCallback(
    (id: string) => {
      selectQuote(id);
      setExpanded(true);
    },
    [setExpanded],
  );
  const handleSort = useCallback(
    (column: string) =>
      setSort(previous => ({
        column,
        direction:
          previous.column === column
            ? previous.direction === "descending"
              ? "ascending"
              : "descending"
            : column === "合约"
              ? "ascending"
              : "descending",
      })),
    [setSort],
  );
  const table = (groupRows: QuoteRow[]) => (
    <QuoteTable
      overviewStyle={shown.length > 1 || groupRows.length < 100}
      columns={visibleColumns}
      nameFor={displayName}
      market={market}
      rows={groupRows}
      selected={listed ? rowKey(listed) : undefined}
      onSelect={selectQuote}
      onOpen={handleOpen}
      sort={sort}
      onSort={handleSort}
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
    <div className="futures-market-board" ref={board} data-expanded={expanded || undefined}>
      <div className="futures-market-board-list" ref={list}>
        {watchlist && <WatchlistEditor context={context} />}
        {!rows.length && (
          <p className="market-search-empty" role="status">
            {t(
              watchlist && !watchlist.length && !query
                ? "还没有自选合约"
                : quotes.length
                  ? "没有匹配的市场合约"
                  : "尚无订阅行情",
            )}
          </p>
        )}
        <div className="market-exchange-groups">
          {boardColumns(
            shown.map(group => group.rows.length + 2),
            Math.max(1, Math.floor(width / columnWidth)),
          ).map(column => (
            <div className="market-exchange-column" key={column[0]}>
              {column.map(index => (
                <section key={shown[index]!.title} aria-label={shown[index]!.label}>
                  <h2>{shown[index]!.label}</h2>
                  {table(shown[index]!.rows)}
                </section>
              ))}
            </div>
          ))}
        </div>
      </div>
      {active && (
        <aside className="market-contract" aria-label={t("合约详情")}>
          <QuotePane
            context={context}
            market={market}
            row={active}
            name={
              (active === listed
                ? displayName(active)
                : contractName(active.instrument!, catalog)) ?? active.instrument!.symbol
            }
            pane="market-pane"
            initial="1分"
            actions={
              <>
                <button
                  type="button"
                  className="market-expand"
                  onClick={() =>
                    context.navigate("workspace.trading", {
                      params: {
                        contract: rowKey(active),
                      },
                    })
                  }
                >
                  {t("下单")}
                </button>
                <WatchlistToggle context={context} row={active.instrument!} />
                <button
                  type="button"
                  className="market-expand"
                  aria-pressed={expanded}
                  onClick={() => setExpanded(value => !value)}
                >
                  {t(expanded ? "收起" : "展开")}
                </button>
              </>
            }
          />
          <ContractQuote market={market} row={active} />
        </aside>
      )}
    </div>
  );
}
