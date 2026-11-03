import type { Page } from "@playwright/test";

type Instrument = { venue: string; symbol: string };
// A market.proto Subscription in ProtoJSON, with a quote when one is given.
export const watchRow = (id: Instrument, quote?: Record<string, unknown>) => ({
  instrument: id,
  state: "subscribed",
  ...(quote && { quote: { instrument: id, tradingDay: "20261013", ...quote } }),
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
    if (request.method === "market.watch")
      return route.fulfill({ json: { result: { market_watch: next(request.params.restart) } } });
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.market_endpoint?.host) body.result.market_endpoint.instruments = watchlist;
    await route.fulfill({ response, json: body });
  });
}
