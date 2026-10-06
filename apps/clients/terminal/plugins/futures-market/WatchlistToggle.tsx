import { useState } from "react";
import type { LiveMarket } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

// Adds the contract to the watchlist or removes it, wherever the contract is
// shown. Membership belongs to the market service, so it needs a live login.
export function WatchlistToggle({
  context,
  market,
  row,
}: {
  context: TerminalContext;
  market: LiveMarket;
  row: { venue: string; symbol: string };
}) {
  const [error, setError] = useState<DisplayError>("");
  const ids = market.watchlist ?? [];
  const same = (id: { venue: string; symbol: string }) =>
    id.venue === row.venue && id.symbol === row.symbol;
  const present = ids.some(same);
  return (
    <>
      <button
        className="watchlist-toggle"
        aria-pressed={present}
        title={row.symbol}
        disabled={context.busy || !market.transport_online || market.phase !== "connected"}
        onClick={() => {
          setError("");
          context
            .trade("market.subscribe", {
              instruments: present
                ? ids.filter(id => !same(id))
                : [...ids, { venue: row.venue, symbol: row.symbol }],
            })
            .catch((reason: unknown) => setError(asDisplayError(reason)));
        }}
      >
        {t(present ? "移出自选" : "加入自选")}
      </button>
      {error && (
        <span role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
        </span>
      )}
    </>
  );
}
