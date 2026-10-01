import type { LiveMarket } from "../../src/bridge/client";

// This is a view of real month contracts, not a provider's continuous series.
// Catalog product identity is authoritative; never infer products from symbols.
export function productOverview(market: LiveMarket) {
  const products = new Map(
    market.catalog.contracts.map(contract => [
      `${contract.venue}.${contract.symbol}`,
      `${contract.venue}.${contract.product}`,
    ]),
  );
  const groups = new Map<string, LiveMarket["subscriptions"][number]>();
  const interest = (row: LiveMarket["subscriptions"][number]) => {
    const value = Number(row.quote?.open_interest);
    return row.quote?.open_interest != null && Number.isFinite(value) ? value : -1;
  };
  for (const row of market.subscriptions) {
    const key = products.get(`${row.venue}.${row.symbol}`);
    if (!key) continue;
    const previous = groups.get(key);
    if (
      !previous ||
      interest(row) > interest(previous) ||
      (interest(row) === interest(previous) &&
        (Number(row.quote?.volume ?? -1) > Number(previous.quote?.volume ?? -1) ||
          (Number(row.quote?.volume ?? -1) === Number(previous.quote?.volume ?? -1) &&
            row.symbol.localeCompare(previous.symbol) < 0)))
    )
      groups.set(key, row);
  }
  return [...groups.values()];
}
