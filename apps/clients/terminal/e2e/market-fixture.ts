import type { Page } from "@playwright/test";

type Instrument = { exchange: string; symbol: string };
const decimals = new Set(["last", "open", "high", "low", "previousSettlement", "openInterest"]);
// A quotes.proto QuoteRow in ProtoJSON, with a quote when one is given; prices
// are given as text.
export const watchRow = (id: Instrument, quote?: Record<string, string>) => ({
  instrument: id,
  subscription: "SUBSCRIPTION_ACTIVE",
  ...(quote && {
    quote: Object.fromEntries([
      ["tradingDay", "2026-10-13"],
      ...Object.entries(quote).map(([key, value]) => [
        key,
        decimals.has(key) ? { text: value } : value,
      ]),
    ]),
  }),
});
// A full or changed quote frame with a connected feed and no contract catalog.
export const board = (rows: object[], catalog = true) => ({
  feed: { state: "FEED_STATE_CONNECTED" },
  rows,
  ...(catalog && { catalog: { status: { state: "CATALOG_STATE_NONE" } } }),
});

// Replaces the frames of the open Market watch with the test's own stream and
// the saved watchlist with the test's list; every other command reaches Native.
export async function serveWatch(
  page: Page,
  next: (restart: boolean) => { frames: object[]; online: boolean },
  watchlist: Instrument[],
) {
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method === "quotes.watch")
      return route.fulfill({ json: { result: { quotes_watch: next(request.params.restart) } } });
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.quotes_endpoint?.host) body.result.quotes_endpoint.instruments = watchlist;
    await route.fulfill({ response, json: body });
  });
}
