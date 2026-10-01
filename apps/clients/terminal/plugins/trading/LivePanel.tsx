import { useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  saveCostTemplate,
  translate,
  useWorkspaceDraft,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import type { LiveSession, TerminalCommand } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);

const phases: Record<LiveSession["phase"], string> = {
  disconnected: "未连接",
  connecting: "连接前置",
  authenticating: "终端认证",
  logging_in: "登录中",
  confirming: "确认结算单",
  synchronizing: "同步账户",
  ready: "已就绪",
  error: "连接失败",
};
const statuses: Record<LiveSession["orders"][number]["status"], string> = {
  submitted: "已报送",
  accepted: "已接受",
  partially_filled: "部分成交",
  filled: "已成交",
  cancelled: "已撤单",
  rejected: "已拒绝",
};
const offsets: Record<string, string> = {
  open: "开仓",
  close_today: "平今",
  close_yesterday: "平昨",
  close: "平仓",
};
const key = (item: { venue: string; symbol: string }) => `${item.venue}.${item.symbol}`;

// Live CTP trading. Orders go to a real futures account only after the owner
// connects with credentials typed here, confirms the account, and the order
// passes the allowed contracts, exchange units and pre-trade risk.
export function LivePanel({ snapshot, busy, trade }: TerminalContext) {
  const live = snapshot?.live?.session ?? null;
  const [error, setError] = useState<DisplayError>("");
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  return (
    <section className="futures-data paper-trading" aria-label={t("期货实盘交易")}>
      <div className="panel-heading">
        <h2>{t("期货实盘交易")}</h2>
        <span className="panel-spacer" />
        <small>{t("CTP · 真实账户")}</small>
      </div>
      <p className="alert">{t("实盘会连接真实期货账户并发送真实委托，成交与盈亏均为真实结果。")}</p>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {snapshot?.live && !live ? (
        <p className="alert">{t("实盘会话服务尚未初始化，请关闭后从原目录恢复。")}</p>
      ) : live ? (
        <LiveAccount live={live} snapshot={snapshot} busy={busy} run={run} />
      ) : (
        <CreateLive snapshot={snapshot} busy={busy} run={run} setError={setError} />
      )}
    </section>
  );
}

type Run = (method: TerminalCommand, params?: Record<string, unknown>) => Promise<boolean>;

function CreateLive({
  snapshot,
  busy,
  run,
  setError,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  run: Run;
  setError: (error: DisplayError) => void;
}) {
  const [directory, setDirectory] = useWorkspaceDraft("live-directory", "");
  const [broker, setBroker] = useWorkspaceDraft("live-broker", {
    front: "",
    broker_id: "",
    user_id: "",
    app_id: "",
  });
  const [limits, setLimits] = useWorkspaceDraft("live-risk", {
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>("live-contracts", []);
  const [choice, setChoice] = useState("");
  const catalog = snapshot?.market?.catalog;
  const listed = catalog?.phase === "ready" ? catalog.contracts : [];
  async function selectDirectory() {
    try {
      const value = await open({
        directory: true,
        multiple: false,
        defaultPath: directory || undefined,
      });
      if (typeof value === "string") setDirectory(value);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  return (
    <form
      onSubmit={event => {
        event.preventDefault();
        void run("live.create", {
          directory,
          ...broker,
          ...limits,
          contracts: contracts.map(item => {
            const [venue, ...symbol] = item.split(".");
            return { venue, symbol: symbol.join(".") };
          }),
        });
      }}
    >
      <fieldset disabled={busy}>
        <div className="futures-file">
          <label>
            {t("实盘记录目录")}
            <input
              aria-label={t("实盘记录目录")}
              value={directory}
              onChange={event => setDirectory(event.target.value)}
              placeholder={t("已存在的专用空目录；恢复时选择原目录")}
              required
            />
          </label>
          {nativeDesktop && (
            <button type="button" onClick={() => void selectDirectory()}>
              {t("选择目录")}
            </button>
          )}
          <button
            type="button"
            disabled={!directory}
            onClick={() => void run("live.open", { directory })}
          >
            {t("恢复实盘会话")}
          </button>
        </div>
        <section className="account-field-group" aria-label={t("CTP 账户")}>
          <h3>{t("CTP 账户")}</h3>
          <div className="futures-fields">
            {(
              [
                ["front", "交易前置地址"],
                ["broker_id", "经纪商代码"],
                ["user_id", "投资者账号"],
                ["app_id", "AppID"],
              ] as const
            ).map(([field, label]) => (
              <label key={field}>
                {t(label)}
                <input
                  aria-label={t(label)}
                  value={broker[field]}
                  placeholder={field === "front" ? "tcp://host:port" : undefined}
                  onChange={event => setBroker({ ...broker, [field]: event.target.value })}
                  required
                />
              </label>
            ))}
          </div>
          <p className="subtle">{t("密码与授权码在每次连接时输入，不保存。")}</p>
        </section>
        <section className="account-field-group" aria-label={t("可交易合约")}>
          <h3>{t("可交易合约")}</h3>
          {catalog?.phase !== "ready" ? (
            <p className="subtle">
              {t("需要先在行情工作区加载 CTP 合约目录；合约单位以目录为准。")}
            </p>
          ) : (
            <div className="futures-file">
              <label>
                {t("合约")}
                <input
                  aria-label={t("添加合约")}
                  list="live-catalog"
                  value={choice}
                  placeholder="SHFE.rb2610"
                  onChange={event => setChoice(event.target.value)}
                />
              </label>
              <datalist id="live-catalog">
                {listed.map(item => (
                  <option key={key(item)} value={key(item)}>
                    {item.name}
                  </option>
                ))}
              </datalist>
              <button
                type="button"
                disabled={
                  !listed.some(item => key(item) === choice) ||
                  contracts.includes(choice) ||
                  contracts.length >= 20
                }
                onClick={() => {
                  setContracts([...contracts, choice]);
                  setChoice("");
                }}
              >
                {t("添加")}
              </button>
            </div>
          )}
          <ul className="dataset-list" aria-label={t("已选可交易合约")}>
            {contracts.map(item => (
              <li key={item}>
                <p>
                  <strong>{item}</strong>{" "}
                  <button
                    type="button"
                    aria-label={t("移除 {contract}", { contract: item })}
                    onClick={() => setContracts(contracts.filter(other => other !== item))}
                  >
                    {t("移除")}
                  </button>
                </p>
              </li>
            ))}
          </ul>
        </section>
        <section className="account-field-group" aria-label={t("委托与持仓限制")}>
          <h3>{t("委托与持仓限制")}</h3>
          <div className="futures-fields">
            {(
              [
                ["max_order_quantity", "单笔数量上限"],
                ["max_gross_quantity", "总持仓量上限"],
                ["max_working_orders", "在途委托数上限"],
                ["max_price_deviation", "价格偏离上限"],
              ] as const
            ).map(([field, label]) => (
              <label key={field}>
                {t(label)}
                <input
                  aria-label={t(label)}
                  inputMode="decimal"
                  placeholder={field === "max_price_deviation" ? "0.02" : undefined}
                  value={limits[field]}
                  onChange={event => setLimits({ ...limits, [field]: event.target.value })}
                  required
                />
              </label>
            ))}
          </div>
          <p className="subtle">
            {t(
              "限价须在涨跌停范围内，且与券商最新价（当日无成交时为昨结算价）的偏离不超过该比例，例如 0.02 表示 2%。每笔委托发出前查询一次行情，约需 1 秒。",
            )}
          </p>
        </section>
        <div className="source-actions">
          <button className="primary" type="submit" disabled={!contracts.length}>
            {t("创建实盘会话")}
          </button>
        </div>
      </fieldset>
    </form>
  );
}

function LiveAccount({
  live,
  snapshot,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "busy"> & { live: LiveSession; run: Run }) {
  const [credentials, setCredentials] = useState({ password: "", auth_code: "" });
  const [confirmed, setConfirmed] = useState(false);
  const [order, setOrder] = useState({
    contract: "",
    side: "buy",
    offset: "open",
    quantity: "1",
    price: "",
  });
  const ready = live.phase === "ready";
  const stale =
    live.storage_state === "recovery_required" ||
    snapshot?.live?.connection.state === "disconnected";
  const authorized = !!live.authorization && live.authorization.trading_day === live.trading_day;
  const traded = live.contracts.find(item => key(item) === order.contract) ?? live.contracts[0];
  // Mirrors the core's ClosePolicy: SHFE/INE (and unverified venues) need
  // explicit today/yesterday closes; the rest assign buckets themselves.
  const explicitBuckets = !["CFFEX", "DCE", "CZCE", "GFEX"].includes(traded?.venue ?? "");
  const offset =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const act = (params: Record<string, unknown>) =>
    run("live.act", { request_id: crypto.randomUUID(), ...params });
  return (
    <>
      <div className="paper-toolbar">
        <strong>
          {live.broker.broker_id} · {live.broker.user_id}
        </strong>
        <span data-testid="live-phase">{t(phases[live.phase])}</span>
        {live.trading_day && <span>{t("交易日 {day}", { day: live.trading_day })}</span>}
        <span className="panel-spacer" />
        {live.phase !== "disconnected" && (
          <button disabled={busy} onClick={() => void run("live.disconnect")}>
            {t("断开账户")}
          </button>
        )}
        <button disabled={busy} onClick={() => void run("live.close")}>
          {t("关闭会话")}
        </button>
      </div>
      {stale && (
        <p className="alert" role="alert">
          {t("实盘服务连接或记录状态不确定。请关闭会话并从原目录恢复；断线命令不会自动重发。")}
        </p>
      )}
      {live.phase === "error" && (
        <p className="alert" role="alert">
          {t("CTP 返回错误 {code}；核对账户信息后重新连接。", { code: live.error_code })}
        </p>
      )}
      {live.unconfirmed.length > 0 && (
        <p className="alert" role="alert">
          {t(
            "{n} 笔已记录的委托没有出现在券商回报中：{ids}。它们不会被重发，并继续占用在途委托额度；请在券商端核实。",
            { n: live.unconfirmed.length, ids: live.unconfirmed.map(item => item.id).join("、") },
          )}
        </p>
      )}
      {(live.phase === "disconnected" || live.phase === "error") && (
        <form
          aria-label={t("连接账户")}
          onSubmit={event => {
            event.preventDefault();
            const sent = credentials;
            setCredentials({ password: "", auth_code: "" });
            void run("live.connect", sent);
          }}
        >
          <fieldset disabled={busy || stale}>
            <div className="futures-fields">
              <label>
                {t("交易密码")}
                <input
                  aria-label={t("交易密码")}
                  type="password"
                  autoComplete="off"
                  value={credentials.password}
                  onChange={event =>
                    setCredentials({ ...credentials, password: event.target.value })
                  }
                  required
                />
              </label>
              <label>
                {t("授权码")}
                <input
                  aria-label={t("授权码")}
                  type="password"
                  autoComplete="off"
                  value={credentials.auth_code}
                  onChange={event =>
                    setCredentials({ ...credentials, auth_code: event.target.value })
                  }
                  required
                />
              </label>
            </div>
            <div className="source-actions">
              <button className="primary" type="submit">
                {t("连接账户")}
              </button>
              <span className="subtle">{t("密码与授权码只用于本次连接，不保存。")}</span>
            </div>
          </fieldset>
        </form>
      )}
      {ready && (
        <section className="account-field-group live-authorization" aria-label={t("实盘授权")}>
          <h3>{t("实盘授权")}</h3>
          {authorized ? (
            <div className="source-actions">
              <span>{t("已授权 · 交易日 {day}", { day: live.authorization!.trading_day })}</span>
              <button disabled={busy} onClick={() => void act({ action: "live_revoke" })}>
                {t("撤销授权")}
              </button>
            </div>
          ) : (
            <>
              <label className="checkbox">
                <input
                  type="checkbox"
                  checked={confirmed}
                  onChange={event => setConfirmed(event.target.checked)}
                />
                {t("我确认使用账户 {user} 发送真实委托", { user: live.broker.user_id })}
              </label>
              <div className="source-actions">
                <button
                  className="primary"
                  disabled={busy || !confirmed || stale}
                  onClick={() => {
                    setConfirmed(false);
                    void act({ action: "live_authorize", user_id: live.broker.user_id });
                  }}
                >
                  {t("授权实盘交易")}
                </button>
                <span className="subtle">{t("授权只在本次连接和当前交易日有效。")}</span>
              </div>
            </>
          )}
        </section>
      )}
      {ready && <AccountRates live={live} busy={busy} run={run} />}
      {live.funds && (
        <dl className="paper-metrics">
          {(
            [
              ["balance", "动态权益"],
              ["available", "可用资金"],
              ["margin", "占用保证金"],
              ["position_profit", "持仓盈亏"],
              ["close_profit", "平仓盈亏"],
              ["commission", "手续费"],
            ] as const
          ).map(([field, label]) => (
            <div key={field}>
              <dt>{t(label)}</dt>
              <dd data-testid={`live-${field}`}>{live.funds![field]}</dd>
            </div>
          ))}
        </dl>
      )}
      <form
        aria-label={t("实盘委托")}
        onSubmit={event => {
          event.preventDefault();
          if (!traded) return;
          void act({
            action: "submit",
            order_id: crypto.randomUUID(),
            venue: traded.venue,
            symbol: traded.symbol,
            side: order.side,
            offset,
            quantity: order.quantity,
            price: order.price,
          });
        }}
      >
        <fieldset disabled={busy || !ready || !authorized || stale}>
          <div className="futures-fields">
            <label>
              {t("合约")}
              <select
                aria-label={t("委托合约")}
                value={traded ? key(traded) : ""}
                onChange={event => setOrder({ ...order, contract: event.target.value })}
              >
                {live.contracts.map(item => (
                  <option key={key(item)} value={key(item)}>
                    {item.venue} · {item.symbol}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("买卖方向")}
              <select
                aria-label={t("买卖方向")}
                value={order.side}
                onChange={event => setOrder({ ...order, side: event.target.value })}
              >
                <option value="buy">{t("买入")}</option>
                <option value="sell">{t("卖出")}</option>
              </select>
            </label>
            <label>
              {t("开平仓")}
              <select
                aria-label={t("开平仓")}
                value={offset}
                onChange={event => setOrder({ ...order, offset: event.target.value })}
              >
                <option value="open">{t("开仓")}</option>
                {explicitBuckets ? (
                  <>
                    <option value="close_today">{t("平今")}</option>
                    <option value="close_yesterday">{t("平昨")}</option>
                  </>
                ) : (
                  <option value="close">{t("平仓")}</option>
                )}
              </select>
            </label>
            <label>
              {t("委托手数")}
              <input
                aria-label={t("委托手数")}
                value={order.quantity}
                onChange={event => setOrder({ ...order, quantity: event.target.value })}
                required
              />
            </label>
            <label>
              {t("限价")}
              <input
                aria-label={t("限价")}
                value={order.price}
                placeholder={traded ? t("最小变动 {tick}", { tick: traded.price_increment }) : ""}
                onChange={event => setOrder({ ...order, price: event.target.value })}
                required
              />
            </label>
          </div>
          <div className="source-actions">
            <button className="primary" type="submit">
              {t("提交实盘委托")}
            </button>
            <span className="subtle">
              {t("委托先写入本机交易记录再发送；断线后不会自动重发。")}
            </span>
          </div>
        </fieldset>
      </form>
      <h3 className="paper-heading">{t("持仓")}</h3>
      <div className="paper-table" role="region" aria-label={t("实盘持仓")} tabIndex={0}>
        <table aria-label={t("实盘持仓")}>
          <thead>
            <tr>
              <th>{t("合约")}</th>
              <th>{t("方向")}</th>
              <th>{t("今仓")}</th>
              <th>{t("昨仓")}</th>
            </tr>
          </thead>
          <tbody>
            {live.positions.map(p => (
              <tr key={`${key(p)}.${p.side}`}>
                <td>{p.symbol}</td>
                <td>{p.side === "buy" ? t("多头") : t("空头")}</td>
                <td>{p.today}</td>
                <td>{p.yesterday}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <h3 className="paper-heading">{t("委托")}</h3>
      <div className="paper-table" role="region" aria-label={t("实盘委托记录")} tabIndex={0}>
        <table aria-label={t("实盘委托记录")}>
          <thead>
            <tr>
              <th>{t("合约")}</th>
              <th>{t("方向 / 开平")}</th>
              <th>{t("限价")}</th>
              <th>{t("手数")}</th>
              <th>{t("已成交")}</th>
              <th>{t("状态")}</th>
              <th>{t("操作")}</th>
            </tr>
          </thead>
          <tbody>
            {live.orders
              .slice()
              .reverse()
              .map(o => (
                <tr key={o.broker_key}>
                  <td>{o.symbol}</td>
                  <td>
                    {o.side === "buy" ? t("买入") : t("卖出")} / {t(offsets[o.offset])}
                  </td>
                  <td>{o.limit_price}</td>
                  <td>{o.quantity}</td>
                  <td>{o.filled}</td>
                  <td>
                    {t(statuses[o.status])}
                    {o.error_code ? ` (${o.error_code})` : ""}
                  </td>
                  <td>
                    {o.id && ["submitted", "accepted", "partially_filled"].includes(o.status) && (
                      <button
                        disabled={busy || !ready || stale}
                        onClick={() => void act({ action: "cancel", order_id: o.id })}
                      >
                        {t("撤单")}
                      </button>
                    )}
                  </td>
                </tr>
              ))}
          </tbody>
        </table>
      </div>
      <h3 className="paper-heading">{t("成交")}</h3>
      <div className="paper-table" role="region" aria-label={t("实盘成交")} tabIndex={0}>
        <table aria-label={t("实盘成交")}>
          <thead>
            <tr>
              <th>{t("合约")}</th>
              <th>{t("时间")}</th>
              <th>{t("价格")}</th>
              <th>{t("手数")}</th>
            </tr>
          </thead>
          <tbody>
            {live.trades
              .slice()
              .reverse()
              .map(f => (
                <tr key={f.id}>
                  <td>{f.symbol}</td>
                  <td>{f.trade_time}</td>
                  <td>{f.price}</td>
                  <td>{f.quantity}</td>
                </tr>
              ))}
          </tbody>
        </table>
      </div>
      <details>
        <summary>{t("风险限制")}</summary>
        <p>
          {t("单笔数量上限")}: {live.risk.max_order_quantity} · {t("总持仓量上限")}:{" "}
          {live.risk.max_gross_quantity} · {t("在途委托数上限")}: {live.risk.max_working_orders} ·{" "}
          {t("价格偏离上限")}: {live.max_price_deviation}
        </p>
        <p>
          {t("可交易合约")}: {live.contracts.map(item => key(item)).join("、")}
        </p>
      </details>
    </>
  );
}

// The account's margin and commission rates as the broker reports them. A
// row can become the product's fee template for paper trading and backtests.
function AccountRates({ live, busy, run }: { live: LiveSession; busy: boolean; run: Run }) {
  const [saved, setSaved] = useState<string[]>([]);
  const [error, setError] = useState<DisplayError | null>(null);
  const day = live.trading_day.replace(/^(\d{4})(\d{2})(\d{2})$/, "$1-$2-$3");
  const states: Record<LiveSession["costs"][number]["state"], string> = {
    querying: "查询中",
    ready: "已返回",
    unavailable: "券商未提供",
  };
  return (
    <section className="account-field-group live-authorization" aria-label={t("账户费率")}>
      <h3>{t("账户费率")}</h3>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      <div className="source-actions">
        <button disabled={busy} onClick={() => void run("live.costs")}>
          {t("查询账户费率")}
        </button>
        <span className="subtle">
          {t("券商对本账户实际收取的保证金率与手续费；多空保证金不同时取较高者。")}
        </span>
      </div>
      {live.costs.length > 0 && (
        <div className="paper-table" role="region" aria-label={t("账户费率表")} tabIndex={0}>
          <table aria-label={t("账户费率表")}>
            <thead>
              <tr>
                <th>{t("合约")}</th>
                <th>{t("保证金 每手 / 比例")}</th>
                <th>{t("开仓 每手 / 比例")}</th>
                <th>{t("平今 每手 / 比例")}</th>
                <th>{t("平昨 每手 / 比例")}</th>
                <th>{t("状态")}</th>
                <th>{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {live.costs.map(row => {
                const contract = live.contracts.find(item => key(item) === key(row));
                const product = contract ? `${contract.venue}/${contract.product}` : "";
                const c = row.costs;
                const versionKey = `${product}@${day}`;
                return (
                  <tr key={key(row)}>
                    <td>{row.symbol}</td>
                    <td>{c && `${c.margin_per_lot} / ${c.margin_rate}`}</td>
                    <td>{c && `${c.open_fee} / ${c.open_fee_rate}`}</td>
                    <td>{c && `${c.close_today_fee} / ${c.close_today_fee_rate}`}</td>
                    <td>{c && `${c.close_yesterday_fee} / ${c.close_yesterday_fee_rate}`}</td>
                    <td>
                      {t(states[row.state])}
                      {row.error_code ? ` (${row.error_code})` : ""}
                    </td>
                    <td>
                      {c && product && (
                        <button
                          disabled={saved.includes(versionKey)}
                          onClick={() => {
                            try {
                              saveCostTemplate(
                                product,
                                c,
                                t("CTP 账户 {broker}/{user}", {
                                  broker: live.broker.broker_id,
                                  user: live.broker.user_id,
                                }),
                                day,
                              );
                              setSaved([...saved, versionKey]);
                              setError(null);
                            } catch (reason) {
                              setError(asDisplayError(reason));
                            }
                          }}
                        >
                          {saved.includes(versionKey)
                            ? t("已存为 {product} 模板", { product })
                            : t("存为 {product} 费率模板", { product })}
                        </button>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
