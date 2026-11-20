import { Subscription } from "../../src/generated/asterion/market_pb";
import { useMarket, useMarketStatus } from "../../src/host/market";
import { getLocale, translate } from "../../src/i18n";

const t = (key: string) => translate("asterion.terminal.futures-market", key);
// Beijing time; the bar shows the time of day and the date on hover.
const clock = (value: bigint, date: boolean) =>
  new Date(Number(value))[date ? "toLocaleString" : "toLocaleTimeString"]("sv-SE", {
    timeZone: "Asia/Shanghai",
    hour12: false,
  });

// What the quote stream does not carry, polled through src/host/market.
export function MarketStatus() {
  const market = useMarket();
  const status = useMarketStatus(Boolean(market?.online));

  if (!status) return null;
  // Both counts from the same stream, so they always agree.
  const rows = market?.board.rows ?? [];
  const confirmed = rows.filter(row => row.subscription === Subscription.ACTIVE).length;
  return (
    <>
      <dl className="market-service-status">
        <div>
          <dt>{t("已订阅")}</dt>
          <dd>
            {confirmed.toLocaleString(getLocale())} / {rows.length.toLocaleString(getLocale())}
          </dd>
        </div>
        <div>
          <dt>{t("最后报价")}</dt>
          <dd title={status.lastQuoteMs ? clock(status.lastQuoteMs, true) : undefined}>
            {status.lastQuoteMs ? clock(status.lastQuoteMs, false) : "—"}
          </dd>
        </div>
      </dl>
    </>
  );
}
