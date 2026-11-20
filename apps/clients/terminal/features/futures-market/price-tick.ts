import type { MarketView } from "../../src/host/market";

// The contract's minimum price move from Market's catalog, as decimal text.
export const priceTickOf = (market: MarketView | null, exchange: string, symbol: string) =>
  market?.board.catalog?.contracts.find(
    item =>
      item.contract === symbol ||
      (item.instrument?.exchange === exchange &&
        item.instrument.symbol.toUpperCase() === symbol.toUpperCase()),
  )?.priceTick?.text;
