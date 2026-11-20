import { useState } from "react";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarket } from "../../src/host/market";
import { type MessageValues, translate } from "../../src/i18n";
import { Icon } from "../../src/ui/Icon";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

// Adds a listed month to the watchlist by its code, or removes one.
export function WatchlistEditor({ context }: { context: TerminalContext }) {
  const market = useMarket();
  const [exchange, setExchange] = useState("SHFE");
  const [symbol, setSymbol] = useState("");
  const { error, attempt } = useActionError();
  const saved = context.snapshot?.market_endpoint;
  const watched = saved && "host" in saved ? saved.instruments : [];
  const canonicalSymbol = (venue: string, raw: string) => {
    const trimmed = raw.trim();
    const listed = market?.board.catalog?.contracts.find(
      item =>
        item.instrument?.exchange === venue &&
        item.instrument.symbol.toUpperCase() === trimmed.toUpperCase(),
    )?.instrument?.symbol;
    if (listed) return listed;
    return venue === "CZCE" || venue === "CFFEX" ? trimmed.toUpperCase() : trimmed.toLowerCase();
  };
  async function select(instruments: { exchange: string; symbol: string }[]) {
    if (
      instruments.length > 50 ||
      instruments.some(i => !/^[A-Za-z]{1,3}\d{3,4}$/.test(i.symbol)) ||
      new Set(instruments.map(i => `${i.exchange}.${i.symbol.toUpperCase()}`)).size !==
        instruments.length
    )
      throw Error(t("请输入不重复的实际月份合约，最多 50 个。"));
    await context.trade("market.watchlist", { instruments });
    setSymbol("");
  }
  return (
    <div className="watchlist-editor">
      <form
        onSubmit={event => {
          event.preventDefault();
          void attempt(() =>
            select([...watched, { exchange, symbol: canonicalSymbol(exchange, symbol) }]),
          );
        }}
      >
        <select
          aria-label={t("交易所")}
          value={exchange}
          onChange={event => setExchange(event.target.value)}
        >
          {["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"].map(value => (
            <option key={value}>{value}</option>
          ))}
        </select>
        <input
          aria-label={t("实际合约")}
          placeholder="rb2610"
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={symbol}
          onChange={event => setSymbol(event.target.value)}
          required
        />
        <button type="submit" disabled={context.busy}>
          {t("添加自选")}
        </button>
      </form>
      {watched.map(item => (
        <span key={`${item.exchange}.${item.symbol}`}>
          {item.exchange} · {item.symbol}
          <button
            type="button"
            disabled={context.busy}
            aria-label={t("移除 {symbol}", { symbol: item.symbol })}
            onClick={() =>
              void attempt(() =>
                select(
                  watched.filter(
                    other => other.exchange !== item.exchange || other.symbol !== item.symbol,
                  ),
                ),
              )
            }
          >
            <Icon name="close" size={12} />
          </button>
        </span>
      ))}
      <ActionError error={error} namespace="asterion.terminal.futures-market" />
    </div>
  );
}
