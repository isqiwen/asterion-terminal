import { FeedState } from "../../src/generated/asterion/history_pb";
import type { MarketView } from "../../src/host/market";
import { ActionError, translate, useActionError, type TerminalContext } from "../contract";
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
  row: { exchange: string; symbol: string };
}) {
  const saved = context.snapshot?.market_endpoint;
  const ids = saved && "instruments" in saved ? saved.instruments : [];
  const same = (id: { exchange: string; symbol: string }) =>
    id.exchange === row.exchange && id.symbol === row.symbol;
  const present = ids.some(same);
  const { error, attempt } = useActionError();
  return (
    <>
      <button
        className="watchlist-toggle"
        aria-pressed={present}
        title={row.symbol}
        disabled={
          context.busy || !market.online || market.board.feed?.state !== FeedState.CONNECTED
        }
        onClick={() =>
          void attempt(() =>
            context.trade("market.watchlist", {
              instruments: present
                ? ids.filter(id => !same(id))
                : [...ids, { exchange: row.exchange, symbol: row.symbol }],
            }),
          )
        }
      >
        {t(present ? "移出自选" : "加入自选")}
      </button>
      <ActionError error={error} namespace="asterion.terminal.futures-market" />
    </>
  );
}
