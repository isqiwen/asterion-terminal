import { MarketBoard } from "./MarketBoard";
import { useMarket } from "../../src/host/market";
import { useState } from "react";
import {
  ErrorNotice,
  BackendError,
  translate,
  type MessageValues,
  type TerminalContext,
  notify,
} from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

export function LivePanel({ context }: { context: TerminalContext }) {
  const [venue, setVenue] = useState("SHFE"),
    [symbol, setSymbol] = useState("");
  const market = useMarket();
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const watched = endpoint?.instruments ?? [];
  const phase = market ? (market.online ? market.snapshot.phase : "unreachable") : "closed";
  const phases: Record<string, string> = {
    closed: t("尚未连接行情服务"),
    disconnected: t("服务尚未连接柜台"),
    connecting: t("连接中"),
    logging_in: t("登录中"),
    connected: t("已连接"),
    reconnecting: t("重连中"),
    initializing: t("初始化中"),
    error: t("连接失败"),
    unreachable: t("行情服务失联"),
  };
  async function run(action: () => Promise<void>) {
    try {
      await action();
    } catch (reason) {
      notify(reason, { namespace: "asterion.terminal.futures-market" });
    }
  }
  async function select(instruments: { venue: string; symbol: string }[]) {
    if (
      instruments.length > 50 ||
      instruments.some(i => !/^[A-Za-z]{1,3}\d{3,4}$/.test(i.symbol)) ||
      new Set(instruments.map(i => `${i.venue}.${i.symbol}`)).size !== instruments.length
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
          <span role="status">{phases[phase] ?? phase}</span>
          <span className="panel-spacer" />
          <button
            disabled={context.busy || (!market && !endpoint)}
            onClick={() => void run(() => context.trade(market ? "market.close" : "market.open"))}
          >
            {t(market ? "关闭行情连接" : "连接行情服务")}
          </button>
        </div>
        <p role="status" aria-label={t("行情来源")} className="market-source">
          {endpoint ? `${endpoint.host}:${endpoint.port}` : t("尚未填写行情服务地址。")}{" "}
          <button onClick={() => context.openSettings("sources")}>{t("配置行情服务")}</button>
        </p>
        <p className="subtle">{t("关闭此连接不影响服务录制或其他客户端。")}</p>
        {!!market?.snapshot.errorCode && (
          <p role="alert">
            {t("行情连接异常，请检查配置或服务时段。")} CTP {market.snapshot.errorCode}
          </p>
        )}
        {market?.snapshot.catalog?.phase === "error" && (
          <ErrorNotice
            error={
              new BackendError(
                market.snapshot.catalog.errorCode,
                market.snapshot.catalog.diagnostic,
              )
            }
          />
        )}
        <details className="market-config">
          <summary>{t("按代码添加自选")}</summary>
          <form
            className="market-watchlist"
            onSubmit={event => {
              event.preventDefault();
              void run(() => select([...watched, { venue, symbol: symbol.trim() }]));
            }}
          >
            <label>
              {t("交易所")}
              <select
                aria-label={t("交易所")}
                value={venue}
                onChange={event => setVenue(event.target.value)}
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
              <span key={`${item.venue}.${item.symbol}`}>
                {item.venue} · {item.symbol}{" "}
                <button
                  disabled={context.busy}
                  aria-label={t("移除 {symbol}", { symbol: item.symbol })}
                  onClick={() =>
                    void run(() =>
                      select(
                        watched.filter(
                          other => other.venue !== item.venue || other.symbol !== item.symbol,
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
        {market && (
          <details className="market-config">
            <summary>{t("连接详情")}</summary>
            <p>{market.snapshot.instanceId}</p>
            <p>{t("忽略乱序报价：{count}", { count: String(market.snapshot.outOfOrder) })}</p>
          </details>
        )}
      </div>
    </div>
  );
  return (
    <section className="live-market" aria-label={t("实时期货行情")}>
      {market ? (
        <MarketBoard market={market} context={context} toolbar={toolbar} />
      ) : (
        <>
          {toolbar}
          <div className="workspace-empty">
            <strong>{t("尚无订阅行情")}</strong>
            <p>{t("连接远程行情服务以查看市场。")}</p>
          </div>
        </>
      )}
    </section>
  );
}
