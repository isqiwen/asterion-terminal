import type { MarketView } from "../../src/host/market";
import { translate, type TerminalContext, notify } from "../contract";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

// Adds the contract to the watchlist or removes it, wherever the contract is
// shown. Membership belongs to the market service, so it needs a live login.
export function WatchlistToggle({
  context,
  market,
  row,
}: {
  context: TerminalContext;
  market: MarketView;
  row: { venue: string; symbol: string };
}) {
  const saved = context.snapshot?.market_endpoint;
  const ids = saved && "instruments" in saved ? saved.instruments : [];
  const same = (id: { venue: string; symbol: string }) =>
    id.venue === row.venue && id.symbol === row.symbol;
  const present = ids.some(same);
  return (
    <>
      <button
        className="watchlist-toggle"
        aria-pressed={present}
        title={row.symbol}
        disabled={context.busy || !market.online || market.snapshot.phase !== "connected"}
        onClick={() => {
          context
            .trade("market.watchlist", {
              instruments: present
                ? ids.filter(id => !same(id))
                : [...ids, { venue: row.venue, symbol: row.symbol }],
            })
            .catch((reason: unknown) =>
              notify(reason, { namespace: "asterion.terminal.futures-market" }),
            );
        }}
      >
        {t(present ? "移出自选" : "加入自选")}
      </button>
    </>
  );
}
