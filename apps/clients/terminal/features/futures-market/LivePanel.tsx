import { MarketBoard } from "./MarketBoard";
import { MarketUnavailable } from "./MarketUnavailable";
import { MarketStatus } from "./MarketStatus";
import { CatalogState, FeedState } from "../../src/generated/asterion/market_pb";
import { useMarket } from "../../src/host/market";
import { useState } from "react";
import {
  ActionError,
  ErrorNotice,
  BackendError,
  translate,
  useActionError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

export function LivePanel({ context }: { context: TerminalContext }) {
  const [exchange, setExchange] = useState("SHFE"),
    [symbol, setSymbol] = useState("");
  const market = useMarket();
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const watched = endpoint?.instruments ?? [];
  const feed = market?.board.feed;
  const states: Partial<Record<FeedState, string>> = {
    [FeedState.NO_SDK]: t("服务尚未连接柜台"),
    [FeedState.DISCONNECTED]: t("服务尚未连接柜台"),
    [FeedState.CONNECTING]: t("连接中"),
    [FeedState.LOGGING_IN]: t("登录中"),
    [FeedState.CONNECTED]: t("已连接"),
    [FeedState.RECONNECTING]: t("重连中"),
    [FeedState.FAILED]: t("连接失败"),
  };
  const phase = !market?.online
    ? t("行情服务失联")
    : (states[feed?.state ?? FeedState.UNSPECIFIED] ?? FeedState[feed?.state ?? 0]);
  const { error, attempt } = useActionError();
  const failed = <ActionError error={error} namespace="asterion.terminal.futures-market" />;
  async function select(instruments: { exchange: string; symbol: string }[]) {
    if (
      instruments.length > 50 ||
      instruments.some(i => !/^[A-Za-z]{1,3}\d{3,4}$/.test(i.symbol)) ||
      new Set(instruments.map(i => `${i.exchange}.${i.symbol}`)).size !== instruments.length
    )
      throw Error(t("请输入不重复的实际月份合约，最多 50 个。"));
    await context.trade("market.watchlist", { instruments });
    setSymbol("");
  }
  const toolbar = (
    <div className="market-toolbar">
      <div className="market-session">
        <div className="panel-heading">
          <h2>{t("实时行情")}</h2>
          <span role="status">{phase}</span>
          <span className="panel-spacer" />
          <span className="subtle">{endpoint && `${endpoint.host}:${endpoint.port}`}</span>
          <button
            disabled={context.busy}
            title={t("关闭此连接不影响服务或其他客户端。")}
            onClick={() => void attempt(() => context.trade("market.close"))}
          >
            {t("关闭行情连接")}
          </button>
        </div>
        <MarketStatus />
        {failed}
        {!!market?.error && (
          <p role="alert">
            {t("行情连接失败")}: {market.error}
          </p>
        )}
        {!!feed?.ctpError && (
          <p role="alert">
            {t("行情连接异常，请检查配置或服务时段。")} CTP {feed.ctpError}
          </p>
        )}
        {market?.board.catalog?.status?.state === CatalogState.FAILED && (
          <ErrorNotice
            error={new BackendError("operation_failed", market.board.catalog.status.error)}
          />
        )}
        <details className="market-config">
          <summary>{t("按代码添加自选")}</summary>
          <form
            className="market-watchlist"
            onSubmit={event => {
              event.preventDefault();
              void attempt(() => select([...watched, { exchange, symbol: symbol.trim() }]));
            }}
          >
            <label>
              {t("交易所")}
              <select
                aria-label={t("交易所")}
                value={exchange}
                onChange={event => setExchange(event.target.value)}
              >
                {["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"].map(value => (
                  <option key={value}>{value}</option>
                ))}
              </select>
            </label>
            <label>
              {t("实际合约")}
              <input
                aria-label={t("实际合约")}
                placeholder="rb2610"
                value={symbol}
                onChange={event => setSymbol(event.target.value)}
                required
              />
            </label>
            <button disabled={context.busy || !endpoint}>{t("添加自选")}</button>
          </form>
          <div className="market-symbols">
            {watched.map(item => (
              <span key={`${item.exchange}.${item.symbol}`}>
                {item.exchange} · {item.symbol}{" "}
                <button
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
                  ×
                </button>
              </span>
            ))}
          </div>
        </details>
      </div>
    </div>
  );
  return (
    <section className="live-market" aria-label={t("实时期货行情")}>
      {market ? (
        <MarketBoard market={market} context={context} toolbar={toolbar} />
      ) : (
        <MarketUnavailable context={context} />
      )}
    </section>
  );
}
