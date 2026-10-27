import type { LiveAction } from "../../src/bridge/commands";
import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import { StrategyStart, StrategyStatus } from "./StrategyRun";
import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import {
  ErrorNotice,
  explicitCloseBuckets,
  asDisplayError,
  saveCostTemplate,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { CtpConnection, LiveSession, Snapshot } from "../../src/bridge/client";
import { t, phases, statuses, offsets, key, type Run } from "./live-shared";

export function LiveAccount({
  account,
  live,
  connection,
  snapshot,
  busy,
  query,
  run,
}: Pick<TerminalContext, "snapshot" | "busy" | "query"> & {
  account: CtpConnection;
  live: LiveSession;
  connection: Snapshot["live"][string]["connection"];
  run: Run;
}) {
  const [credentials, setCredentials] = useState({ password: "", auth_code: "" });
  const [activity, setActivity] = useState<Activity>("positions");
  const identity = JSON.stringify([
    connection.session,
    connection.host,
    connection.port,
    live.broker.front,
    live.broker.broker_id,
    live.broker.user_id,
  ]);
  const environment = live.environment;
  const [environmentConfirmed, setEnvironmentConfirmed] = useWorkspaceDraft(
    `ctp-environment-confirmed:${identity}`,
    false,
  );
  const environmentReady = environmentConfirmed;

  const [confirmedPolicy, setConfirmedPolicy] = useState<string | null>(null);
  const confirmed = confirmedPolicy === live.policy_revision;
  const [order, setOrder] = useState<{
    contract: string;
    offset: Extract<LiveAction, { action: "submit" }>["offset"];
    quantity: string;
    price: string;
  }>({
    contract: "",
    offset: "open",
    quantity: "1",
    price: "",
  });
  const ready = live.phase === "ready";
  const stale = live.storage_state === "recovery_required" || connection.state === "disconnected";
  const authorized = !!live.authorization;
  const traded = live.contracts.find(item => key(item) === order.contract) ?? live.contracts[0];
  const explicitBuckets = explicitCloseBuckets(snapshot, traded?.venue ?? "");
  const offset =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const act = (params: LiveAction) =>
    run("live.act", {
      request_id: crypto.randomUUID(),
      account_id: live.account_id,
      policy_revision: live.policy_revision,
      ...params,
    });
  // One step is offered at a time: the counter connection, then permission to
  // send, then the order ticket. A window that has not confirmed the
  // environment of an already connected account confirms it first.
  const stage = stale
    ? "stale"
    : live.phase === "disconnected" || live.phase === "error"
      ? "connect"
      : !ready
        ? "connecting"
        : !environmentReady
          ? "environment"
          : !authorized
            ? "authorize"
            : "trade";
  const quote = snapshot?.market?.subscriptions.find(
    row => row.venue === traded?.venue && row.symbol === traded?.symbol,
  )?.quote;
  // Buying and selling are separate buttons that name the account, so
  // pressing Enter in a field never sends an order.
  const submit = (side: "buy" | "sell", form: HTMLFormElement | null) => {
    if (!traded || !form?.reportValidity()) return;
    void act({
      action: "submit",
      order_id: crypto.randomUUID(),
      venue: traded.venue,
      symbol: traded.symbol,
      side,
      offset,
      quantity: order.quantity,
      price: order.price,
    }).then(sent => sent && setActivity("orders"));
  };
  const environmentFields = (
    <div className="environment-check">
      <label>
        {t("柜台环境")}
        <span>{t(environment === "real" ? "真实资金" : "CTP 仿真")}</span>
      </label>
      <label className="checkbox">
        <input
          type="checkbox"
          required
          checked={environmentConfirmed}
          onChange={e => setEnvironmentConfirmed(e.target.checked)}
        />
        {t("我已核对账户与柜台环境")}
      </label>
      <p className="subtle">
        {t("请向开户机构核对账号及前置地址。环境由你确认，系统无法自动验证资金性质。")}
      </p>
    </div>
  );
  // What takes something back is never held back by another command, of this
  // account or of the window: a cancel, a strategy stop and a revoke are each
  // disabled only while the same one is on its way.
  const [cancelling, setCancelling] = useState<readonly string[]>([]);
  const [revoking, setRevoking] = useState(false);
  // New orders come from the owner or from one strategy run, never both.
  const [origin, setOrigin] = useState<"manual" | "strategy">("manual");
  const [stopping, setStopping] = useState(false);
  const controlled = live.strategy?.state === "running";
  const cancel = async (id: string) => {
    setCancelling(current => [...current, id]);
    try {
      await act({ action: "cancel", order_id: id });
    } finally {
      setCancelling(current => current.filter(item => item !== id));
    }
  };
  return (
    <>
      <div className="ctp-identity">
        <strong>
          {account.name} · {live.broker.broker_id} · {live.broker.user_id}
        </strong>
        <strong>
          {t(
            environmentReady && environment === "real"
              ? "真实资金 · 用户确认"
              : environmentReady && environment === "simulation"
                ? "CTP 仿真 · 用户确认"
                : "CTP 环境待确认",
          )}
        </strong>
        <span data-testid="live-phase">{t(stale ? "服务失联" : phases[live.phase])}</span>
        <span>{authorized ? t("已授权") : t("尚未允许发送委托")}</span>
        <span>{live.broker.front}</span>
        {/* The account's actions stay together when the line wraps. */}
        <div className="ctp-identity-actions">
          {authorized && (
            <button
              disabled={revoking}
              onClick={() => {
                setRevoking(true);
                void act({ action: "live_revoke" }).finally(() => setRevoking(false));
              }}
            >
              {t("撤销授权")}
            </button>
          )}
          {live.phase !== "disconnected" && (
            <button
              disabled={busy}
              title={t("断开账户会退出柜台连接，已有委托不会自动撤销。")}
              onClick={() => void run("live.disconnect")}
            >
              {t("断开账户")}
            </button>
          )}
          {stale && (
            <button
              className="primary"
              disabled={busy}
              onClick={() => void run("live.close").then(closed => closed && run("live.open"))}
            >
              {t("重新连接交易服务")}
            </button>
          )}
        </div>
      </div>
      <details>
        <summary>{t("柜台观察范围")}</summary>
        <dl className="definition-list">
          <dt>{t("连接代次")}</dt>
          <dd>{live.connection_generation}</dd>
          <dt>{t("观察序号")}</dt>
          <dd>{live.broker_sequence}</dd>
          <dt>{t("持仓核对状态")}</dt>
          <dd>{t(live.positions_reconciled ? "已纳入已知成交" : "尚未核对已知成交")}</dd>
        </dl>
        <p className="subtle">
          {t("资金、持仓、订单和成交可能来自不同时间；同步完成时间不代表原子账本或完整历史。")}
        </p>
      </details>
      {environmentReady && (
        <p className={environment === "real" ? "alert" : "subtle"}>
          {t(
            environment === "real"
              ? "真实资金账户：授权后的委托可能产生真实成交与盈亏。"
              : "已标记为 CTP 仿真；委托仍会发送到上方柜台，请确认地址正确。",
          )}
        </p>
      )}
      {stale && (
        <p className="alert" role="alert">
          {t("交易服务连接或记录状态不确定。请重新连接交易服务；断线命令不会自动重发。")}
        </p>
      )}
      {live.phase === "error" && (
        <p className="alert" role="alert">
          {live.error_code === -1008
            ? t("柜台回报队列已满，新增报单已停止。请重新连接并核对账户；已有委托不会自动撤销。")
            : t("CTP 返回错误 {code}；核对账户信息后重新连接。", { code: live.error_code })}
        </p>
      )}
      {live.unconfirmed.length > 0 && (
        <section className="alert" role="alert" aria-label={t("未确认委托")}>
          <p>
            {t(
              "{n} 笔已记录的委托没有出现在券商回报中：{ids}。它们不会被重发，并继续占用在途委托额度；请在券商端核实。",
              { n: live.unconfirmed.length, ids: live.unconfirmed.map(item => item.id).join("、") },
            )}
          </p>
          <p>{t("在券商端确认某笔委托不存在后，可将其标为已核实，它不再占用在途委托额度。")}</p>
          <div className="source-actions">
            {live.unconfirmed.map(item => (
              <button
                key={item.id}
                disabled={busy || stale}
                onClick={() => void act({ action: "live_resolve", order_id: item.id })}
              >
                {t("已核实券商无此委托：{id}", { id: item.id })}
              </button>
            ))}
          </div>
        </section>
      )}
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
      {stage !== "stale" && stage !== "trade" && (
        <FlowSteps
          labels={[t("核对并连接"), t("允许发送委托"), t("下单")]}
          current={stage === "authorize" ? 1 : 0}
        />
      )}
      {stage === "connect" && (
        <form
          aria-label={t("连接账户")}
          onSubmit={event => {
            event.preventDefault();
            if (!environmentReady) return;
            const sent = credentials;
            setCredentials({ password: "", auth_code: "" });
            void run("live.connect", sent);
          }}
        >
          <fieldset disabled={busy}>
            {environmentFields}
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
                  maxLength={16}
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
      {stage === "connecting" && (
        <p role="status">{t("正在连接柜台：{phase}", { phase: t(phases[live.phase]) })}</p>
      )}
      {stage === "environment" && (
        <section aria-label={t("柜台环境")}>
          <p>{t("账户已连接。此窗口尚未核对柜台环境，核对后才能授权和下单。")}</p>
          {environmentFields}
        </section>
      )}
      {stage === "authorize" && (
        <section className="trading-permission" aria-label={t("交易授权")}>
          <label className="checkbox">
            <input
              type="checkbox"
              checked={confirmed}
              onChange={event =>
                setConfirmedPolicy(event.target.checked ? live.policy_revision : null)
              }
            />
            {t("我确认使用账户 {user} 向上方柜台发送委托", { user: live.broker.user_id })}
          </label>
          <div className="source-actions">
            <button
              className="primary"
              disabled={busy || !confirmed}
              onClick={() => {
                setConfirmedPolicy(null);
                void act({ action: "live_authorize", user_id: live.broker.user_id });
              }}
            >
              {t("允许发送委托")}
            </button>
            <span className="subtle">
              {t(
                "授权长期有效，断线重连和换交易日后不需要再次授权；撤销授权、修改风控政策或交易服务重启后失效。",
              )}
            </span>
          </div>
        </section>
      )}
      {/* A run outlives the connection: it is shown, and can be stopped, at every step. */}
      {controlled && (
        <StrategyStatus
          live={live}
          stopping={stopping}
          onStop={() => {
            setStopping(true);
            void act({ action: "strategy_stop" }).finally(() => setStopping(false));
          }}
        />
      )}
      {stage === "trade" && !controlled && (
        <div className="activity-tabs" role="group" aria-label={t("下单方式")}>
          <button aria-pressed={origin === "manual"} onClick={() => setOrigin("manual")}>
            {t("手动下单")}
          </button>
          <button aria-pressed={origin === "strategy"} onClick={() => setOrigin("strategy")}>
            {t("策略运行")}
          </button>
        </div>
      )}
      {stage === "trade" &&
        !controlled &&
        origin === "strategy" &&
        connection.transport !== "local" && (
          <p className="subtle">{t("远程交易实例暂不支持自动策略，请使用手动交易。")}</p>
        )}
      {stage === "trade" &&
        !controlled &&
        origin === "strategy" &&
        connection.transport === "local" && (
          <StrategyStart
            live={live}
            busy={busy}
            query={query}
            onStart={packageId =>
              void run("live.strategy.start", {
                request_id: crypto.randomUUID(),
                account_id: live.account_id,
                policy_revision: live.policy_revision,
                package: packageId,
              })
            }
          />
        )}
      {stage === "trade" && !controlled && origin === "manual" && (
        <form aria-label={t("CTP 委托")} onSubmit={event => event.preventDefault()}>
          <fieldset disabled={busy}>
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
                {t("开平仓")}
                <select
                  aria-label={t("开平仓")}
                  value={offset}
                  onChange={event =>
                    setOrder({ ...order, offset: event.target.value as typeof order.offset })
                  }
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
                  inputMode="numeric"
                  value={order.quantity}
                  onChange={event => setOrder({ ...order, quantity: event.target.value })}
                  required
                />
              </label>
              <label>
                {t("限价")}
                <input
                  aria-label={t("限价")}
                  inputMode="decimal"
                  value={order.price}
                  placeholder={traded ? t("最小变动 {tick}", { tick: traded.price_increment }) : ""}
                  onChange={event => setOrder({ ...order, price: event.target.value })}
                  required
                />
              </label>
            </div>
            {quote && (
              <div className="order-quotes" role="group" aria-label={t("按行情填入限价")}>
                {(
                  [
                    ["last", "最新 {price}"],
                    ["bid", "买一 {price}"],
                    ["ask", "卖一 {price}"],
                  ] as const
                ).map(
                  ([field, label]) =>
                    quote[field] && (
                      <button
                        key={field}
                        type="button"
                        onClick={() => setOrder({ ...order, price: quote[field]! })}
                      >
                        {t(label, { price: quote[field]! })}
                      </button>
                    ),
                )}
              </div>
            )}
            <div className="source-actions">
              {(["buy", "sell"] as const).map(side => (
                <button
                  key={side}
                  type="button"
                  className="primary"
                  onClick={event => submit(side, event.currentTarget.form)}
                >
                  {t(side === "buy" ? "买入" : "卖出")} <small>{account.name}</small>
                </button>
              ))}
              <span className="subtle">
                {t("委托先写入本机交易记录再发送；断线后不会自动重发。")}
              </span>
            </div>
          </fieldset>
        </form>
      )}
      <ActivityTabs value={activity} onChange={setActivity} />
      <div hidden={activity !== "positions"}>
        <div className="paper-table" role="region" aria-label={t("CTP 持仓")} tabIndex={0}>
          <table aria-label={t("CTP 持仓")}>
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
          {!live.positions.length && <p className="subtle">{t("暂无持仓")}</p>}
        </div>
      </div>
      <div hidden={activity !== "orders"}>
        <div className="paper-table" role="region" aria-label={t("CTP 委托记录")} tabIndex={0}>
          <table aria-label={t("CTP 委托记录")}>
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
                          disabled={
                            cancelling.includes(o.id) ||
                            stale ||
                            !(ready || (live.phase === "error" && live.error_code === -1008))
                          }
                          onClick={() => void cancel(o.id)}
                        >
                          {t("撤单")}
                        </button>
                      )}
                    </td>
                  </tr>
                ))}
            </tbody>
          </table>
          {!live.orders.length && <p className="subtle">{t("暂无委托")}</p>}
        </div>
      </div>
      <div hidden={activity !== "fills"}>
        <div className="paper-table" role="region" aria-label={t("CTP 成交")} tabIndex={0}>
          <table aria-label={t("CTP 成交")}>
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
          {!live.trades.length && <p className="subtle">{t("暂无成交")}</p>}
        </div>
      </div>
      <div hidden={activity !== "rates"}>
        {ready ? (
          <AccountRates live={live} busy={busy} run={run} />
        ) : (
          <p className="subtle">{t("连接柜台后可查询账户费率。")}</p>
        )}
      </div>
      <div hidden={activity !== "policy"}>
        <section aria-label={t("风险限制")}>
          <h3>{t("风险限制")}</h3>
          <p>
            {t("单笔数量上限")}: {live.risk.max_order_quantity} · {t("总持仓量上限")}:{" "}
            {live.risk.max_gross_quantity} · {t("在途委托数上限")}: {live.risk.max_working_orders} ·{" "}
            {t("价格偏离上限")}: {live.max_price_deviation}
          </p>
          <p>
            {t("可交易合约")}: {live.contracts.map(item => key(item)).join("、")}
          </p>
        </section>
        <AccountPolicyEditor
          key={live.policy_revision}
          live={live}
          snapshot={snapshot}
          disabled={busy || stale}
          run={run}
        />
      </div>
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
    <section className="account-rates" aria-label={t("账户费率")}>
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
