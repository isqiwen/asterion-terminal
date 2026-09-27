import { QuoteTable } from "./QuoteTable";
import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  getLocale,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
type Instrument = { venue: string; symbol: string };
type Profile = { front: string; broker: string; user: string; instruments: Instrument[] };
const storage = "asterion.ctp-market.profile.v1";
function read(): { profile: Profile; error: DisplayError } {
  const blank = { front: "", broker: "", user: "", instruments: [] };
  try {
    const raw = localStorage.getItem(storage);
    if (!raw) return { profile: blank, error: "" };
    const p = JSON.parse(raw);
    if (
      Object.keys(p).sort().join() !== "broker,front,instruments,user" ||
      ![p.front, p.broker, p.user].every(v => typeof v === "string") ||
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
  const [venue, setVenue] = useState("SHFE"),
    [symbol, setSymbol] = useState("");
  const [error, setError] = useState<DisplayError>(initial.error);
  const market = context.snapshot?.market;
  const online = !!market?.transport_online;
  const phase = market ? (online ? market.phase : "unreachable") : "not_started";
  const idle = !market || ["disconnected", "error", "sdk_unavailable"].includes(market.phase);
  const busy = context.busy;
  const canEdit = idle && !busy && !initial.error;
  const canConnect = canEdit && online && market?.phase !== "sdk_unavailable";
  async function run(action: () => Promise<void>) {
    setError("");
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
  const rows = market?.subscriptions ?? [];
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
  return (
    <section className="live-market" aria-label={t("实时期货行情")}>
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
        <p role="alert" className="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.futures-market" />
        </p>
      )}
      {market?.error_code ? (
        <p role="alert" className="alert">
          {t("行情连接异常，请检查配置或服务时段。")}
          <details>
            <summary>{t("详情")}</summary>CTP {market.error_code}
          </details>
        </p>
      ) : null}
      <details className="market-config" open={idle}>
        <summary>{t("CTP 连接与自选")}</summary>
        <form
          onSubmit={event => {
            event.preventDefault();
            if (!canConnect) return;
            const secret = password;
            setPassword("");
            void run(async () => {
              save(profile);
              await context.trade("market.connect", { ...profile, password: secret });
            });
          }}
        >
          <fieldset disabled={!canEdit}>
            <div className="futures-fields">
              <label>
                {t("行情前置")}
                <input
                  aria-label={t("行情前置")}
                  placeholder="tcp://host:port"
                  required
                  value={profile.front}
                  onChange={e => setProfile({ ...profile, front: e.target.value })}
                />
              </label>
              <label>
                {t("经纪商代码")}
                <input
                  aria-label={t("经纪商代码")}
                  required
                  maxLength={10}
                  value={profile.broker}
                  onChange={e => setProfile({ ...profile, broker: e.target.value })}
                />
              </label>
              <label>
                {t("用户代码")}
                <input
                  aria-label={t("用户代码")}
                  required
                  maxLength={15}
                  value={profile.user}
                  onChange={e => setProfile({ ...profile, user: e.target.value })}
                />
              </label>
              <label>
                {t("密码")}
                <input
                  aria-label={t("密码")}
                  type="password"
                  autoComplete="off"
                  required
                  maxLength={40}
                  value={password}
                  onChange={e => setPassword(e.target.value)}
                />
              </label>
            </div>
            <button type="submit" disabled={!canConnect}>
              {t("连接行情")}
            </button>
          </fieldset>
        </form>
        <form
          className="market-watchlist"
          onSubmit={event => {
            event.preventDefault();
            void run(() => watchlist([...watched, { venue, symbol: symbol.trim() }]));
          }}
        >
          <label>
            {t("交易所")}
            <select aria-label={t("交易所")} value={venue} onChange={e => setVenue(e.target.value)}>
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
        <p className="subtle">{t("只读行情 · 密码不保存 · 使用服务方提供的实际月份合约")}</p>
      </details>
      {!rows.length ? (
        <div className="dashboard-empty">
          <strong>{t("尚无订阅行情")}</strong>
          <p>{t("添加实际合约并连接行情服务。")}</p>
        </div>
      ) : (
        market && <QuoteTable market={market} />
      )}
      {market && (
        <details className="market-config">
          <summary>{t("连接详情")}</summary>
          <p>
            {market.service} · {market.instance_id}
          </p>
          <p>{t("忽略乱序报价：{count}", { count: market.out_of_order })}</p>
        </details>
      )}
    </section>
  );
}
