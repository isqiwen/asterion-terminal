import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { translate } from "../../src/i18n";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

// Adds the contract to the local watchlist or removes it, wherever the contract is shown.
export function WatchlistToggle({
  context,
  row,
}: {
  context: TerminalContext;
  row: { exchange: string; symbol: string };
}) {
  const saved = context.snapshot?.market_endpoint;
  const ids = saved && "instruments" in saved ? saved.instruments : [];
  const same = (id: { exchange: string; symbol: string }) =>
    id.exchange === row.exchange && id.symbol.toUpperCase() === row.symbol.toUpperCase();
  const present = ids.some(same);
  const { error, attempt } = useActionError();
  return (
    <>
      <button
        type="button"
        className="watchlist-toggle"
        aria-pressed={present}
        title={row.symbol}
        disabled={context.busy}
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
