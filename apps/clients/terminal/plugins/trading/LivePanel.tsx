import { AccountPolicyEditor } from "./AccountPolicyEditor";
import { ActivityTabs, type Activity } from "./ActivityTabs";
import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState } from "react";
import {
  ErrorNotice,
  explicitCloseBuckets,
  asDisplayError,
  saveCostTemplate,
  translate,
  useWorkspaceDraft,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import { readableCtpConnection } from "../../src/bridge/client";
import type { CtpConnection, LiveSession, TerminalCommand } from "../../src/bridge/client";
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
export function LivePanel(context: TerminalContext) {
  const { snapshot, busy, trade } = context;
  // Every CTP account can trade; several may be open at once.
  const accounts = (snapshot?.ctp_connections ?? []).filter(readableCtpConnection);
  const [chosen, setChosen] = useWorkspaceDraft("live-account", "");
  const account = accounts.find(item => item.id === chosen) ?? accounts[0];
  const [error, setError] = useState<DisplayError>("");
  // Commands always carry the account they were issued for.
  const runFor =
    (id: string): Run =>
    async (method, params = {}) => {
      setError("");
      try {
        await trade(method, { account: id, ...params });
        return true;
      } catch (reason) {
        setError(asDisplayError(reason));
        return false;
      }
    };
  const state = (item: CtpConnection) => {
    const session = snapshot?.live[item.id]?.session;
    if (!session) return item.trading_record ? "服务未启动" : "未开通交易";
    if (session.phase !== "ready") return phases[session.phase];
    return session.authorization?.trading_day === session.trading_day ? "已允许发送委托" : "已就绪";
  };
  const entry = account ? snapshot?.live[account.id] : undefined;
  return (
    <section className="futures-data paper-trading" aria-label={t("CTP 交易账户")}>
      <div className="panel-heading">
        <h2>{t("CTP 交易")}</h2>
        <span className="panel-spacer" />
        <small>{t("柜台仿真 / 真实资金")}</small>
      </div>

      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {!accounts.length ? (
        <div className="workflow-empty">
          <p>{t("还没有 CTP 账户，请先在设置中添加。")}</p>
          <button onClick={() => context.openSettings("ctp")}>{t("管理 CTP 账户")}</button>
        </div>
      ) : (
        <div className="ctp-accounts">
          <nav className="ctp-account-list" aria-label={t("CTP 账户")}>
            {accounts.map(item => (
              <button
                key={item.id}
                aria-current={item.id === account?.id ? "true" : undefined}
                onClick={() => {
                  setError("");
                  setChosen(item.id);
                }}
              >
                <strong>{item.name}</strong>
                <span className="subtle">{t(state(item))}</span>
              </button>
            ))}
            <button className="ctp-account-manage" onClick={() => context.openSettings("ctp")}>
              {t("管理 CTP 账户")}
            </button>
          </nav>
          {account && (
            <div className="ctp-account-panel" key={account.id}>
              {entry?.session ? (
                <LiveAccount
                  account={account}
                  live={entry.session}
                  connection={entry.connection}
                  snapshot={snapshot}
                  busy={busy}
                  run={runFor(account.id)}
                />
              ) : entry ? (
                <p className="alert">{t("交易服务尚未初始化。")}</p>
              ) : account.trading_record ? (
                <div className="workflow-empty">
                  <p>
                    {account.name} · {account.broker_id} · {account.user_id}
                  </p>
                  <button
                    className="primary"
                    disabled={busy}
                    onClick={() => void runFor(account.id)("live.open")}
                  >
                    {t("启动交易服务")}
                  </button>
                  <p className="subtle">
                    {t("此账户的交易服务未在运行。启动只运行服务程序，不登录柜台。")}
                  </p>
                </div>
              ) : (
                <div className="account-setup">
                  <div className="workflow-heading">
                    <h3>{t("开通交易：{name}", { name: account.name })}</h3>
                  </div>
                  <CreateLive
                    account={account}
                    snapshot={snapshot}
                    busy={busy}
                    run={runFor(account.id)}
                    openSettings={context.openSettings}
                  />
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </section>
  );
}

type Run = (method: TerminalCommand, params?: Record<string, unknown>) => Promise<boolean>;

function CreateLive({
  account,
  snapshot,
  busy,
  run,
  openSettings,
}: Pick<TerminalContext, "snapshot" | "busy" | "openSettings"> & {
  account: CtpConnection;
  run: Run;
}) {
  // The record keeps its own copy of the account's counter details.
  const connection = account;
  const [step, setStep] = useWorkspaceDraft(`live-create-step:${account.id}`, 0);
  const [catalogCredentials, setCatalogCredentials] = useState({ password: "", auth_code: "" });
  const [limits, setLimits] = useWorkspaceDraft(`live-risk:${account.id}`, {
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
  });
  const [contracts, setContracts] = useWorkspaceDraft<string[]>(`live-contracts:${account.id}`, []);
  const [choice, setChoice] = useState("");
  const catalog = snapshot?.market?.catalog;
  const listed = catalog?.phase === "ready" ? catalog.contracts : [];
  return (
    <>
      <div className="inline-row">
        <p role="status" aria-label={t("CTP 账户")}>
          {`${connection.name} · ${connection.broker_id} · ${connection.user_id} · ${
            connection.trade_front
          }`}
        </p>
        <button type="button" onClick={() => openSettings("ctp")}>
          {t("管理 CTP 账户")}
        </button>
      </div>
      <p className="subtle">
        {t("开通后柜台信息固定在此账户的交易记录中，不能再修改。")}
        {t("密码与授权码在每次连接时输入，不保存。")}
      </p>
      <FlowSteps labels={[t("合约与风控"), t("确认创建")]} current={step} />
      <form
        onSubmit={event => {
          event.preventDefault();
          if (step === 0) {
            setStep(1);
            return;
          }
          void run("live.create", {
            ...limits,
            contracts: contracts.map(item => {
              const [venue, ...symbol] = item.split(".");
              return { venue, symbol: symbol.join(".") };
            }),
          }).then(created => {
            if (created) setStep(0);
          });
        }}
      >
        <fieldset disabled={busy}>
          <fieldset hidden={step !== 0} disabled={step !== 0}>
            <section className="account-field-group" aria-label={t("可交易合约")}>
              <h3>{t("可交易合约")}</h3>
              {catalog?.phase !== "ready" ? (
                <div>
                  <p className="subtle">
                    {t("先读取柜台合约规格，不会创建委托。凭据仅用于本次查询。")}
                  </p>
                  <div className="futures-fields">
                    <label>
                      {t("交易密码")}
                      <input
                        type="password"
                        autoComplete="off"
                        aria-label={t("目录查询密码")}
                        value={catalogCredentials.password}
                        onChange={e =>
                          setCatalogCredentials({ ...catalogCredentials, password: e.target.value })
                        }
                      />
                    </label>
                    <label>
                      {t("授权码")}
                      <input
                        type="password"
                        autoComplete="off"
                        aria-label={t("目录查询授权码")}
                        maxLength={16}
                        value={catalogCredentials.auth_code}
                        onChange={e =>
                          setCatalogCredentials({
                            ...catalogCredentials,
                            auth_code: e.target.value,
                          })
                        }
                      />
                    </label>
                  </div>
                  <button
                    type="button"
                    disabled={
                      busy ||
                      !catalogCredentials.password ||
                      ["connecting", "authenticating", "logging_in", "querying"].includes(
                        catalog?.phase ?? "",
                      )
                    }
                    onClick={() => {
                      const credentials = catalogCredentials;
                      setCatalogCredentials({ password: "", auth_code: "" });
                      void run("market.catalog", credentials);
                    }}
                  >
                    {t("获取合约列表")}
                  </button>
                  <p role="status">
                    {t(
                      catalog?.phase === "error"
                        ? "合约查询失败，请核对连接信息后重试。"
                        : "合约规格未就绪",
                    )}
                  </p>
                </div>
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
          </fieldset>
          {step === 1 && (
            <section className="creation-review">
              <dl>
                <dt>{t("CTP 账户")}</dt>
                <dd>
                  <strong>{connection.name}</strong>
                  <span>
                    {connection.broker_id} · {connection.user_id}
                  </span>
                  <span>{connection.trade_front}</span>
                </dd>
                <dt>{t("可交易合约")}</dt>
                <dd>{contracts.join(" + ")}</dd>
                <dt>{t("委托与持仓限制")}</dt>
                <dd>
                  {t("单笔数量上限")}：{limits.max_order_quantity} · {t("总持仓量上限")}：
                  {limits.max_gross_quantity} · {t("在途委托数上限")}：{limits.max_working_orders} ·{" "}
                  {t("价格偏离上限")}：{limits.max_price_deviation}
                </dd>
              </dl>
              <p className="subtle">
                {t("创建账户不会登录或发送委托。下一步确认环境、连接并授权。")}
              </p>
            </section>
          )}
          <div className="workflow-actions">
            {step === 1 && (
              <button type="button" onClick={() => setStep(0)}>
                {t("上一步")}
              </button>
            )}
            <button className="primary" type="submit" disabled={!contracts.length}>
              {t(step === 1 ? "创建 CTP 账户" : "下一步")}
            </button>
          </div>
        </fieldset>
      </form>
    </>
  );
}

function LiveAccount({
  account,
  live,
  connection,
  snapshot,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  account: CtpConnection;
  live: LiveSession;
  connection: { session: string; host?: string; port?: number; state: string };
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
  const [environment, setEnvironment] = useWorkspaceDraft<"unknown" | "simulation" | "real">(
    `ctp-environment:${identity}`,
    "unknown",
  );
  const [environmentConfirmed, setEnvironmentConfirmed] = useWorkspaceDraft(
    `ctp-environment-confirmed:${identity}`,
    false,
  );
  const environmentReady = environment !== "unknown" && environmentConfirmed;

  const [confirmedPolicy, setConfirmedPolicy] = useState<string | null>(null);
  const confirmed = confirmedPolicy === live.policy_revision;
  const [order, setOrder] = useState({
    contract: "",
    offset: "open",
    quantity: "1",
    price: "",
  });
  const ready = live.phase === "ready";
  const stale = live.storage_state === "recovery_required" || connection.state === "disconnected";
  const authorized = !!live.authorization && live.authorization.trading_day === live.trading_day;
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
  const act = (params: Record<string, unknown>) =>
    run("live.act", {
      request_id: crypto.randomUUID(),
      account_id: live.account_id,
      policy_revision: live.policy_revision,
      ...params,
    });
  // A cancel is never held back by another command of this account; only a
  // second click on the same order is.
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
        <select
          aria-label={t("柜台环境")}
          required
          value={environment === "unknown" ? "" : environment}
          onChange={e => {
            setEnvironment((e.target.value || "unknown") as typeof environment);
            setEnvironmentConfirmed(false);
          }}
        >
          <option value="">{t("请选择并核对")}</option>
          <option value="simulation">{t("CTP 仿真")}</option>
          <option value="real">{t("真实资金")}</option>
        </select>
      </label>
      <label className="checkbox">
        <input
          type="checkbox"
          required
          checked={environmentConfirmed}
          disabled={environment === "unknown"}
          onChange={e => setEnvironmentConfirmed(e.target.checked)}
        />
        {t("我已核对账户与柜台环境")}
      </label>
      <p className="subtle">
        {t("请向开户机构核对账号及前置地址。环境由你确认，系统无法自动验证资金性质。")}
      </p>
    </div>
  );
  const [cancelling, setCancelling] = useState<readonly string[]>([]);
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
        <span>
          {authorized
            ? t("已授权 · 交易日 {day}", { day: live.authorization!.trading_day })
            : t("尚未允许发送委托")}
        </span>
        <span>{live.broker.front}</span>
        <span className="panel-spacer" />
        {authorized && (
          <button disabled={busy} onClick={() => void act({ action: "live_revoke" })}>
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
            <span className="subtle">{t("授权只在本次连接和当前交易日有效。")}</span>
          </div>
        </section>
      )}
      {stage === "trade" && (
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
