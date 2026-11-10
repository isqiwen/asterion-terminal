import type { MarketView } from "../../src/host/market";

// The contract's minimum price move from Market's catalog, as decimal text.
export const priceTickOf = (market: MarketView | null, exchange: string, symbol: string) =>
  market?.board.catalog?.contracts.find(
    item => item.instrument?.exchange === exchange && item.instrument.symbol === symbol,
  )?.priceTick?.text;
