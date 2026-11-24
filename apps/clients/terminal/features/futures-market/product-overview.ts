import type { Catalog, QuoteRow } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";

const catalogProducts = new WeakMap<Catalog, Map<string, string>>();

function productsOf(catalog: Catalog | undefined) {
  if (!catalog) return null;
  let map = catalogProducts.get(catalog);
  if (!map) {
    map = new Map(
      catalog.contracts.map(contract => [
        `${contract.instrument?.exchange}.${contract.instrument?.symbol}`,
        `${contract.instrument?.exchange}.${contract.product}`,
      ]),
    );
    catalogProducts.set(catalog, map);
  }
  return map;
}

// This is a view of real month contracts, not a provider's continuous series.
// Catalog product identity is authoritative; never infer products from symbols.
export function productOverview(market: MarketView) {
  const products = productsOf(market.board.catalog);
  if (!products) return [];
  const groups = new Map<string, QuoteRow>();
  const interest = (row: QuoteRow) => {
    const value = Number(row.quote?.openInterest?.text);
    return row.quote?.openInterest && Number.isFinite(value) ? value : -1;
  };
  const volume = (row: QuoteRow) => row.quote?.volume ?? -1n;
  for (const row of market.board.rows) {
    const key = products.get(rowKey(row));
    if (!key) continue;
    const previous = groups.get(key);
    if (
      !previous ||
      interest(row) > interest(previous) ||
      (interest(row) === interest(previous) &&
        (volume(row) > volume(previous) ||
          (volume(row) === volume(previous) &&
            row.instrument!.symbol < previous.instrument!.symbol)))
    )
      groups.set(key, row);
  }
  return [...groups.values()];
}
