import type { Subscription } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";

// This is a view of real month contracts, not a provider's continuous series.
// Catalog product identity is authoritative; never infer products from symbols.
export function productOverview(market: MarketView) {
  const products = new Map(
    (market.snapshot.catalog?.contracts ?? []).map(contract => [
      `${contract.instrument?.venue}.${contract.instrument?.symbol}`,
      `${contract.instrument?.venue}.${contract.product}`,
    ]),
  );
  const groups = new Map<string, Subscription>();
  const interest = (row: Subscription) => {
    const value = Number(row.quote?.openInterest);
    return row.quote?.openInterest != null && Number.isFinite(value) ? value : -1;
  };
  const volume = (row: Subscription) => row.quote?.volume ?? -1n;
  for (const row of market.snapshot.subscriptions) {
    const key = products.get(rowKey(row));
    if (!key) continue;
    const previous = groups.get(key);
    if (
      !previous ||
      interest(row) > interest(previous) ||
      (interest(row) === interest(previous) &&
        (volume(row) > volume(previous) ||
          (volume(row) === volume(previous) &&
            row.instrument!.symbol.localeCompare(previous.instrument!.symbol) < 0)))
    )
      groups.set(key, row);
  }
  return [...groups.values()];
}
