import { useEffect } from "react";
import type { Session } from "../../src/generated/asterion/trading_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { rowKey, type MarketView } from "../../src/host/market";
import { contractName } from "../../src/ui/contract-names";
import { QuotePane } from "../futures-market/QuotePane";
import { QuoteTable } from "../futures-market/QuoteTable";
import { WatchlistToggle } from "../futures-market/WatchlistToggle";
import { findPolicyTerms, t } from "./live-shared";

const columns = ["最新价", "涨跌幅"] as const;

// What the account holds, then the watchlist, beside the chart of the contract
// the order ticket is on. Choosing a row only moves the ticket; it sends nothing.
export function TradeMarket({
  context,
  market,
  session,
  contract,
  select,
}: {
  context: TerminalContext;
  market: MarketView;
  session: Session;
  contract: string;
  select: (contract: string) => void;
}) {
  const saved = context.snapshot?.market_endpoint;
  const watchlist = saved && "instruments" in saved ? saved.instruments : [];
  const { rows: quotes, catalog } = market.board;
  const rows = [
    ...new Set(
      [...session.positions.map(item => item.instrument), ...watchlist].flatMap(id => {
        const row = quotes.find(
          item =>
            item.instrument!.exchange === id?.exchange &&
            item.instrument!.symbol.toUpperCase() === id.symbol.toUpperCase(),
        );
        return row ? [row] : [];
      }),
    ),
  ];
  const preferred = rows.find(item => findPolicyTerms(session.policy?.contracts, item.instrument));
  const firstRow = preferred ?? rows[0];
  const first = firstRow && rowKey(firstRow);
  useEffect(() => {
    if (!contract && first) select(first);
  }, [contract, first, select]);
  const row = quotes.find(item => rowKey(item) === contract);
  return (
    <>
      <div className="trade-watch futures-market-board">
        {rows.length ? (
          <QuoteTable
            overviewStyle
            columns={columns}
            nameFor={item => contractName(item.instrument!, catalog)}
            market={market}
            rows={rows}
            selected={contract}
            onSelect={select}
            onNavigate={(current, direction) => {
              const index = rows.findIndex(item => rowKey(item) === current);
              const next = rows[Math.max(0, Math.min(rows.length - 1, index + direction))];
              if (next) select(rowKey(next));
            }}
          />
        ) : (
          <p className="subtle">{t("还没有自选合约")}</p>
        )}
      </div>
      <div className="trade-chart">
        {row ? (
          <QuotePane
            context={context}
            market={market}
            row={row}
            name={contractName(row.instrument!, catalog) ?? row.instrument!.symbol}
            pane="trade-pane"
            initial="1分"
            actions={<WatchlistToggle context={context} row={row.instrument!} />}
          />
        ) : (
          <p className="market-chart-empty">{t("无行情")}</p>
        )}
      </div>
    </>
  );
}
