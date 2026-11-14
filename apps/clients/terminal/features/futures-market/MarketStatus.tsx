import { useEffect, useState } from "react";
import { Subscription, type Status } from "../../src/generated/asterion/market_pb";
import { useMarket } from "../../src/host/market";
import { marketData } from "../../src/bridge/market-data";
import { getLocale, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { useErrorNotification } from "../../src/host/notifications";

const t = (key: string) => translate("asterion.terminal.futures-market", key);
// Beijing time; the bar shows the time of day and the date on hover.
const clock = (value: bigint, date: boolean) =>
  new Date(Number(value))[date ? "toLocaleString" : "toLocaleTimeString"]("sv-SE", {
    timeZone: "Asia/Shanghai",
    hour12: false,
  });

// What the quote stream does not carry. Reconnecting discards pending replies.
export function MarketStatus() {
  const market = useMarket();
  const online = market?.online;
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<DisplayError>();
  useErrorNotification(error, "asterion.terminal.futures-market");
  useEffect(() => {
    setStatus(null);
    setError(undefined);
    if (!online) return;
    let active = true;
    let pending = false;
    const read = async () => {
      if (pending) return;
      pending = true;
      try {
        const next = await marketData({ case: "getStatus", value: {} });
        if (!active) return;
        setStatus(next);
        setError("");
      } catch (reason) {
        if (active) {
          setStatus(null);
          setError(asDisplayError(reason));
        }
      } finally {
        pending = false;
      }
    };
    void read();
    const timer = window.setInterval(() => void read(), 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [online]);

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
