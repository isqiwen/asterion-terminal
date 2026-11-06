import { useEffect, useState } from "react";
import { FeedState, type Status } from "../../src/generated/asterion/market_pb";
import { useMarket } from "../../src/host/market";
import {
  ActionError,
  ErrorNotice,
  useActionError,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";

const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const feedStates: Partial<Record<FeedState, string>> = {
  [FeedState.CONNECTED]: "行情已连接",
  [FeedState.CONNECTING]: "连接中",
  [FeedState.LOGGING_IN]: "登录中",
  [FeedState.RECONNECTING]: "重连中",
  [FeedState.DISCONNECTED]: "行情未连接",
  [FeedState.NO_SDK]: "行情未连接",
  [FeedState.FAILED]: "行情连接失败",
};
const clock = (value: bigint | undefined) =>
  value === undefined
    ? "—"
    : new Date(Number(value)).toLocaleString(getLocale(), {
        timeZone: "Asia/Shanghai",
        hour12: false,
      });
const size = (bytes: bigint) =>
  `${(Number(bytes / 1024n) / 1024).toLocaleString(getLocale(), { maximumFractionDigits: 1 })} MiB`;

// Only this client's status reads are polled. Changing or closing its Market
// connection discards pending replies without changing the recorder's lifetime.
export function MarketStatus({ context }: { context: TerminalContext }) {
  const market = useMarket();
  const online = market?.online;
  const saved = context.snapshot?.market_endpoint;
  const address = saved && "host" in saved ? `${saved.host}:${saved.port}` : "";
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const { error: opened, attempt } = useActionError();
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
    const timer = window.setInterval(() => {
      if (!document.hidden) void read();
    }, 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [address, online]);

  return (
    <section className="data-card market-status" aria-label={t("行情状态")}>
      <header>
        <h3>{t("行情状态")}</h3>
        {status?.feed && (
          <span role="status" data-state={FeedState[status.feed.state].toLowerCase()}>
            {t(feedStates[status.feed.state] ?? FeedState[status.feed.state])}
          </span>
        )}
      </header>
      {!market ? (
        <div className="data-actions">
          <button
            disabled={context.busy}
            onClick={() => void attempt(() => context.trade("market.open"))}
          >
            {t("连接行情服务")}
          </button>
          <span className="subtle">{t("连接行情服务后查看状态。")}</span>
          <ActionError error={opened} />
        </div>
      ) : !online ? (
        <p role="status">{t("行情服务失联，状态未知。")}</p>
      ) : error ? (
        <ErrorNotice error={error} />
      ) : !status ? (
        <p className="subtle">{t("读取中")}</p>
      ) : (
        <>
          <dl>
            <div>
              <dt>{t("订阅合约")}</dt>
              <dd>{status.subscribed.toLocaleString(getLocale())}</dd>
            </div>
            <div>
              <dt>{t("磁盘可用空间")}</dt>
              <dd>{size(status.diskFreeBytes)}</dd>
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
          {status.catalog?.error && (
            <p role="alert">
              {t("合约目录查询失败")}: {status.catalog.error}
            </p>
          )}
          {!!status.feed?.ctpError && (
            <p role="alert">
              {t("行情连接失败")}: CTP {status.feed.ctpError}
            </p>
          )}
        </>
      )}
    </section>
  );
}
