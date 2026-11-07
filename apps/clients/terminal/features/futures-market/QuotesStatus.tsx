import { useEffect, useState } from "react";
import type { Status } from "../../src/generated/asterion/quotes_pb";
import { useMarket } from "../../src/host/market";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  quotesData,
  translate,
  type DisplayError,
} from "../contract";

const t = (key: string) => translate("asterion.terminal.futures-market", key);
const clock = (value: bigint | undefined) =>
  value === undefined
    ? "—"
    : new Date(Number(value)).toLocaleString(getLocale(), {
        timeZone: "Asia/Shanghai",
        hour12: false,
      });

// What the quote stream does not carry. Reconnecting discards pending replies.
export function QuotesStatus() {
  const online = useMarket()?.online;
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    setStatus(null);
    setError("");
    if (!online) return;
    let active = true;
    let pending = false;
    const read = async () => {
      if (pending) return;
      pending = true;
      try {
        const next = await quotesData({ case: "getStatus", value: {} });
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
    const timer = window.setInterval(() => {
      if (!document.hidden) void read();
    }, 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [online]);

  if (error) return <ErrorNotice error={error} />;
  if (!status) return null;
  return (
    <>
      <dl className="quotes-status">
        <div>
          <dt>{t("订阅合约")}</dt>
          <dd>{status.subscribed.toLocaleString(getLocale())}</dd>
        </div>
        <div>
          <dt>{t("最后收到行情（北京时间）")}</dt>
          <dd>{clock(status.lastQuoteMs)}</dd>
        </div>
      </dl>
      {status.failedSubscriptions > 0 && (
        <p role="alert">
          {t("合约订阅失败，每 5 分钟重试")}:{" "}
          {status.failedSubscriptions.toLocaleString(getLocale())}
        </p>
      )}
    </>
  );
}
