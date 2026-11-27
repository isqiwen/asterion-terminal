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
  const groups = new Map<string, { row: QuoteRow; oi: number; vol: bigint }>();
  for (const row of market.board.rows) {
    const key = products.get(rowKey(row));
    if (!key) continue;
    const rawOi = row.quote?.openInterest ? Number(row.quote.openInterest.text) : -1;
    const oi = Number.isFinite(rawOi) ? rawOi : -1;
    const vol = row.quote?.volume ?? -1n;
    const previous = groups.get(key);
    if (
      !previous ||
      oi > previous.oi ||
      (oi === previous.oi &&
        (vol > previous.vol ||
          (vol === previous.vol && row.instrument!.symbol < previous.row.instrument!.symbol)))
    )
      groups.set(key, { row, oi, vol });
  }
  const result: QuoteRow[] = [];
  for (const entry of groups.values()) result.push(entry.row);
  return result;
}
