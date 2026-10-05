import { MarketBoard } from "./MarketBoard";
import { useEffect, useState } from "react";
import {
  ErrorNotice,
  BackendError,
  asDisplayError,
  translate,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import { marketCtpAccount } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
type Instrument = { venue: string; symbol: string };
// The counter account is the current CTP account from Settings; this panel
// only remembers the watchlist to subscribe at login.
type Profile = { instruments: Instrument[] };
const storage = "asterion.ctp-market.watchlist.v1";
function read(): { profile: Profile; error: DisplayError } {
  const blank = { instruments: [] };
  try {
    const raw = localStorage.getItem(storage);
    if (!raw) return { profile: blank, error: "" };
    const p = JSON.parse(raw);
    if (
      Object.keys(p).join() !== "instruments" ||
      !Array.isArray(p.instruments) ||
      p.instruments.length > 50 ||
      !p.instruments.every(
        (i: Instrument) => typeof i.venue === "string" && typeof i.symbol === "string",
      )
    )
      throw Error();
    return { profile: p, error: "" };
  } catch {
    return { profile: blank, error: t("行情配置无法读取，请检查本机存储。") };
  }
}
export function LivePanel({ context }: { context: TerminalContext }) {
  const [initial] = useState(read);
  const [profile, setProfile] = useState(initial.profile);
  const [password, setPassword] = useState("");
  const [authCode, setAuthCode] = useState("");
  const [catalogPassword, setCatalogPassword] = useState("");
  const [remember, setRemember] = useState(false);
  const [notice, setNotice] = useState("");
  const [venue, setVenue] = useState("SHFE"),
    [symbol, setSymbol] = useState("");
  const [error, setError] = useState<DisplayError>(initial.error);
  const market = context.snapshot?.market;
  const connection = marketCtpAccount(context.snapshot);
  const online = !!market?.transport_online;
  const phase = market ? (online ? market.phase : "unreachable") : "not_started";
  const idle = !market || ["disconnected", "error", "sdk_unavailable"].includes(market.phase);
  const busy = context.busy;
  const canEdit = idle && !busy && !initial.error;
  const canConnect = canEdit && online && market?.phase !== "sdk_unavailable";
  const watchlistKey = JSON.stringify(market?.watchlist ?? []);
  useEffect(() => {
    if (idle || initial.error) return;
    const next = { instruments: JSON.parse(watchlistKey) as Instrument[] };
    try {
      localStorage.setItem(storage, JSON.stringify(next));
    } catch (reason) {
      setError(asDisplayError(reason));
    }
    setProfile(next);
  }, [watchlistKey, idle, initial.error]);
  async function loadCatalog(secret: string, auth = authCode) {
    setCatalogPassword("");
    setAuthCode("");
    await context.trade("market.catalog", {
      account: connection?.id ?? "",
      password: secret,
      auth_code: auth,
    });
  }
  async function run(action: () => Promise<void>) {
    setError("");
    setNotice("");
    try {
      await action();
    } catch (e) {
      setError(asDisplayError(e));
    }
  }
  function save(p: Profile) {
    if (initial.error) throw Error(t("行情配置无法读取，请检查本机存储。"));
    localStorage.setItem(storage, JSON.stringify(p));
    setProfile(p);
  }
  async function watchlist(ids: Instrument[]) {
    if (
      ids.length > 50 ||
      ids.some(i => !/^[A-Za-z]{1,3}\d{3,4}$/.test(i.symbol)) ||
      new Set(ids.map(i => i.symbol)).size !== ids.length
    )
      throw Error(t("请输入不重复的实际月份合约，最多 50 个。"));
    if (market && !idle) await context.trade("market.subscribe", { instruments: ids });
    save({ ...profile, instruments: ids });
    setSymbol("");
  }
  const rows = market?.watchlist ?? [];
  const watched =
    market && !idle ? rows.map(({ venue, symbol }) => ({ venue, symbol })) : profile.instruments;
  const phases: Record<string, string> = {
    not_started: t("行情服务未启动"),
    disconnected: t("未登录"),
    connecting: t("连接中"),
    logging_in: t("登录中"),
    connected: t("已登录"),
    reconnecting: t("重连中"),
    error: t("连接失败"),
    sdk_unavailable: t("当前平台缺少 CTP 行情组件"),
    unreachable: t("行情服务失联"),
  };
  const toolbar = (
    <div className="market-toolbar">
      <div className="market-session">
        <div className="panel-heading">
          <h2>{t("实时行情")}</h2>
          <span role="status">{phases[phase] ?? phase}</span>
          <span className="panel-spacer" />
          {!market && (
            <button disabled={busy} onClick={() => void run(() => context.trade("market.local"))}>
              {t("启动本机行情服务")}
            </button>
          )}
          {market && !idle && (
            <button
              disabled={busy || !online}
              onClick={() => void run(() => context.trade("market.disconnect"))}
            >
              {t("断开行情")}
            </button>
          )}
        </div>
        {error && (
          <div role="alert" className="alert">
            <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
          </div>
        )}
        {notice && <p role="status">{notice}</p>}
        {market?.error_code ? (
          <div role="alert" className="alert">
            {t("行情连接异常，请检查配置或服务时段。")}
            <details>
              <summary>{t("详情")}</summary>CTP {market.error_code}
            </details>
          </div>
        ) : null}
        {market?.catalog?.phase === "loading" && <p role="status">{t("正在获取完整合约目录…")}</p>}
        {market?.catalog?.phase === "cached" && (
          <p role="status">
            {t("合约目录来自 {day} 的本机缓存，连接后更新。", { day: market.catalog.trading_day })}
          </p>
        )}
        {market?.catalog?.phase === "error" && (
          <div role="alert">
            <ErrorNotice
              error={new BackendError(market.catalog.error_code, market.catalog.diagnostic)}
            />
          </div>
        )}
        <details className="market-config" open={idle || !market?.subscriptions.length}>
          <summary>{t("CTP 连接与自选")}</summary>
          <form
            onSubmit={event => {
              event.preventDefault();
              if (!canConnect || !connection) return;
              const secret = password;
              const auth = authCode;
              setPassword("");
              setAuthCode("");
              void run(async () => {
                save(profile);
                if (remember && (secret || auth)) {
                  await context.trade("market.credentials.save", {
                    account: connection.id,
                    password: secret,
                    auth_code: auth,
                  });
                  setNotice(t("登录凭据已保存到本机钥匙串。"));
                }
                await context.trade("market.connect", {
                  password: secret,
                  instruments: profile.instruments,
                });
                await loadCatalog(secret, auth);
              });
            }}
          >
            <p role="status" aria-label={t("行情来源")}>
              {connection
                ? t("行情来源：{name}（{broker} · {user}）", {
                    name: connection.name,
                    broker: connection.broker_id,
                    user: connection.user_id,
                  })
                : t("尚未选择行情账户")}{" "}
              <button type="button" onClick={() => context.openSettings("ctp")}>
                {t("更换")}
              </button>
            </p>
            <fieldset disabled={!canEdit}>
              <div className="futures-fields">
                <label>
                  {t("密码")}
                  <input
                    aria-label={t("密码")}
                    type="password"
                    autoComplete="off"
                    required={remember && !!authCode}
                    placeholder={t("留空使用已保存的凭据")}
                    maxLength={40}
                    value={password}
                    onChange={e => setPassword(e.target.value)}
                  />
                </label>
                {connection && (
                  <label>
                    {t("授权码")}
                    <input
                      aria-label={t("授权码")}
                      maxLength={16}
                      type="password"
                      autoComplete="off"
                      required={remember && !!password}
                      placeholder={t("留空使用已保存的凭据")}
                      value={authCode}
                      onChange={e => setAuthCode(e.target.value)}
                    />
                  </label>
                )}
              </div>
              <label className="market-remember">
                <input
                  type="checkbox"
                  checked={remember}
                  onChange={event => setRemember(event.target.checked)}
                />
                {t("保存密码和授权码到本机钥匙串")}
              </label>
              <button type="submit" disabled={!canConnect || !connection}>
                {t("连接行情")}
              </button>
            </fieldset>
            <button
              type="button"
              disabled={busy || !connection}
              onClick={() =>
                void run(async () => {
                  await context.trade("market.credentials.clear", { account: connection!.id });
                  setPassword("");
                  setAuthCode("");
                  setCatalogPassword("");
                  setRemember(false);
                  setNotice(t("已清除保存的登录凭据。"));
                })
              }
            >
              {t("清除已保存的登录凭据")}
            </button>
            <p className="subtle">
              {t("连接行情后用行情账户的交易前置加载完整合约目录；仅查询合约，不开通交易。")}
            </p>
          </form>
          {!idle && connection && (
            <form
              onSubmit={event => {
                event.preventDefault();
                void run(() => loadCatalog(catalogPassword));
              }}
            >
              <fieldset disabled={busy}>
                <div className="futures-fields">
                  <label>
                    {t("目录查询密码")}
                    <input
                      aria-label={t("目录查询密码")}
                      type="password"
                      autoComplete="off"
                      value={catalogPassword}
                      onChange={e => setCatalogPassword(e.target.value)}
                      placeholder={t("留空使用已保存的凭据")}
                    />
                  </label>
                  <label>
                    {t("授权码")}
                    <input
                      aria-label={t("授权码")}
                      maxLength={16}
                      type="password"
                      autoComplete="off"
                      placeholder={t("留空使用已保存的凭据")}
                      value={authCode}
                      onChange={e => setAuthCode(e.target.value)}
                    />
                  </label>
                </div>
                <button disabled={!online || market?.catalog?.phase === "loading"}>
                  {t("加载完整市场")}
                </button>
              </fieldset>
            </form>
          )}
          <form
            className="market-watchlist"
            onSubmit={event => {
              event.preventDefault();
              void run(() => watchlist([...watched, { venue, symbol: symbol.trim() }]));
            }}
          >
            {phase === "connected" && !market?.subscriptions.length && (
              <p role="status">
                {t("已登录，尚未订阅合约。选择交易所并添加实际月份合约后接收行情。")}
              </p>
            )}
            <label>
              {t("交易所")}
              <select
                aria-label={t("交易所")}
                value={venue}
                onChange={e => setVenue(e.target.value)}
              >
                {["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"].map(v => (
                  <option key={v}>{v}</option>
                ))}
              </select>
            </label>
            <label>
              {t("实际合约")}
              <input
                aria-label={t("实际合约")}
                placeholder="rb2610"
                value={symbol}
                onChange={e => setSymbol(e.target.value)}
                required
              />
            </label>
            <button disabled={busy || !!initial.error || (!!market && !online)}>
              {t("添加自选")}
            </button>
          </form>
          <div className="market-symbols">
            {watched.map(i => (
              <span key={i.symbol}>
                {i.venue} · {i.symbol}{" "}
                <button
                  disabled={busy || (!!market && !online)}
                  aria-label={t("移除 {symbol}", { symbol: i.symbol })}
                  onClick={() =>
                    void run(() => watchlist(watched.filter(v => v.symbol !== i.symbol)))
                  }
                >
                  ×
                </button>
              </span>
            ))}
          </div>
          {market && (
            <details className="market-config">
              <summary>{t("连接详情")}</summary>
              <p>
                {market.service} · {market.instance_id}
              </p>
              <p>{t("忽略乱序报价：{count}", { count: market.out_of_order })}</p>
            </details>
          )}
          <p className="subtle">{t("只读行情 · 使用服务方提供的实际月份合约")}</p>
        </details>
      </div>
    </div>
  );
  return (
    <section className="live-market" aria-label={t("实时期货行情")}>
      {!market ? (
        <>
          {toolbar}
          <div className="workspace-empty">
            <strong>{t("尚无订阅行情")}</strong>
            <p>{t("添加实际合约并连接行情服务。")}</p>
          </div>
        </>
      ) : (
        market && <MarketBoard market={market} context={context} toolbar={toolbar} />
      )}
    </section>
  );
}
