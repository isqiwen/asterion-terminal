import { AccountLibrary } from "./AccountLibrary";
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
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import { currentCtpAccount } from "../../src/bridge/client";
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
export function LivePanel(context: TerminalContext) {
  const { snapshot, busy, trade } = context;
  const [creating, setCreating] = useWorkspaceDraft("live-creating", false);
  const [directory, setDirectory] = useWorkspaceDraft("live-directory", "");
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
      {snapshot?.live && !live ? (
        <p className="alert">{t("实盘会话服务尚未初始化，请关闭后从原目录恢复。")}</p>
      ) : live ? (
        <LiveAccount live={live} snapshot={snapshot} busy={busy} run={run} />
      ) : creating ? (
        <div className="account-setup">
          <div className="workflow-heading">
            <h3>{t("添加 CTP 账户")}</h3>
            <button disabled={busy} onClick={() => setCreating(false)}>
              {t("取消")}
            </button>
          </div>
          <CreateLive
            snapshot={snapshot}
            busy={busy}
            run={run}
            setError={setError}
            onCreated={() => setCreating(false)}
            openSettings={context.openSettings}
          />
        </div>
      ) : (
        <AccountLibrary
          context={context}
          directory={directory}
          setDirectory={setDirectory}
          onCreate={() => setCreating(true)}
          onOpen={value => void run("live.open", { directory: value })}
          onError={reason => setError(asDisplayError(reason))}
        />
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
  onCreated,
  openSettings,
}: Pick<TerminalContext, "snapshot" | "busy" | "openSettings"> & {
  run: Run;
  setError: (error: DisplayError) => void;
  onCreated: () => void;
}) {
  const [step, setStep] = useWorkspaceDraft("live-step", 0);
  const [name, setName] = useWorkspaceDraft("live-name", "");
  const [catalogCredentials, setCatalogCredentials] = useState({ password: "", auth_code: "" });
  const [directory, setDirectory] = useWorkspaceDraft("live-directory", "");
  // Trading uses the current CTP account from Settings. The created record
  // keeps its own copy and does not follow later edits.
  const connection = currentCtpAccount(snapshot);
  const tradable = !!connection?.trade_front && !!connection.app_id;
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
    <>
      <FlowSteps labels={[t("账户信息"), t("合约与风控"), t("确认创建")]} current={step} />
      <form
        onSubmit={event => {
          event.preventDefault();
          if (step < 2) {
            setStep(step + 1);
            return;
          }
          void run("live.create", {
            ...(directory ? { directory } : { name }),
            ...limits,
            contracts: contracts.map(item => {
              const [venue, ...symbol] = item.split(".");
              return { venue, symbol: symbol.join(".") };
            }),
          }).then(created => {
            if (created) {
              setStep(0);
              onCreated();
            }
          });
        }}
      >
        <fieldset disabled={busy}>
          <fieldset hidden={step !== 0} disabled={step !== 0}>
            <label>
              {t("账户名称")}
              <input
                aria-label={t("账户名称")}
                value={name}
                onChange={e => setName(e.target.value)}
                required={!directory}
                maxLength={40}
              />
            </label>
            <details>
              <summary>{t("自定义记录目录")}</summary>
              <p className="subtle">{t("留空时自动创建专用目录，之后从账户列表打开。")}</p>
              <div className="futures-file">
                <label>
                  {t("实盘记录目录")}
                  <input
                    aria-label={t("实盘记录目录")}
                    value={directory}
                    onChange={event => setDirectory(event.target.value)}
                    placeholder={t("已存在的专用空目录；恢复时选择原目录")}
                  />
                </label>
                {nativeDesktop && (
                  <button type="button" onClick={() => void selectDirectory()}>
                    {t("选择目录")}
                  </button>
                )}
              </div>
            </details>
            <section className="account-field-group" aria-label={t("CTP 账户")}>
              <h3>{t("CTP 账户")}</h3>
              <p role="status" aria-label={t("当前 CTP 账户")}>
                {connection
                  ? `${connection.name} · ${connection.broker_id} · ${connection.user_id} · ${
                      connection.trade_front || t("未填写交易前置")
                    }`
                  : t("尚未设置 CTP 账户")}
              </p>
              <button type="button" onClick={() => openSettings("ctp")}>
                {t("管理 CTP 账户")}
              </button>
              <p className="subtle">{t("交易使用设置中的当前 CTP 账户，需要交易前置与 AppID。")}</p>
              <p className="subtle">{t("密码与授权码在每次连接时输入，不保存。")}</p>
            </section>
          </fieldset>
          <fieldset hidden={step !== 1} disabled={step !== 1}>
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
          {step === 2 && (
            <section className="creation-review">
              <h3>{name || connection?.user_id}</h3>
              <p>
                {connection?.broker_id} · {connection?.user_id}
              </p>
              <p>{connection?.trade_front}</p>
              <p>{contracts.join(" + ")}</p>
              <p>
                {t("单笔数量上限")}：{limits.max_order_quantity} · {t("总持仓量上限")}：
                {limits.max_gross_quantity}
              </p>
              <p>{t("创建账户不会登录或发送委托。下一步确认环境、连接并授权。")}</p>
            </section>
          )}
          <div className="workflow-actions">
            {step > 0 && (
              <button type="button" onClick={() => setStep(step - 1)}>
                {t("上一步")}
              </button>
            )}
            <button
              className="primary"
              type="submit"
              disabled={!tradable || (step > 0 && !contracts.length)}
            >
              {t(step === 2 ? "创建 CTP 账户" : "下一步")}
            </button>
          </div>
        </fieldset>
      </form>
    </>
  );
}

function LiveAccount({
  live,
  snapshot,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "busy"> & { live: LiveSession; run: Run }) {
  const [credentials, setCredentials] = useState({ password: "", auth_code: "" });
  const [activity, setActivity] = useState<Activity>("positions");
  const identity = JSON.stringify([
    snapshot?.live?.connection.session,
    snapshot?.live?.connection.host,
    snapshot?.live?.connection.port,
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
    !!snapshot?.stale ||
    live.storage_state === "recovery_required" ||
    snapshot?.live?.connection.state === "disconnected";
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
    run("live.act", { request_id: crypto.randomUUID(), ...params });
  return (
    <>
      <div className="paper-toolbar ctp-identity">
        <strong>
          {live.broker.broker_id} · {live.broker.user_id}
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
        <span>{t(authorized ? "已允许发送委托" : "尚未允许发送委托")}</span>
        <span>{live.broker.front}</span>
        <span className="panel-spacer" />
        {live.phase !== "disconnected" && (
          <button disabled={busy} onClick={() => void run("live.disconnect")}>
            {t("断开账户")}
          </button>
        )}
        <button disabled={busy} onClick={() => void run("live.close")}>
          {t("离开账户")}
        </button>
      </div>
      {(!environmentReady || live.phase === "disconnected" || live.phase === "error") && (
        <section className="environment-check">
          <label>
            {t("柜台环境")}
            <select
              aria-label={t("柜台环境")}
              value={environment}
              onChange={e => {
                setEnvironment(e.target.value as typeof environment);
                setEnvironmentConfirmed(false);
              }}
            >
              <option value="unknown">{t("请选择并核对")}</option>
              <option value="simulation">{t("CTP 仿真")}</option>
              <option value="real">{t("真实资金")}</option>
            </select>
          </label>
          <p className="subtle">
            {t("请向开户机构核对账号及前置地址。环境由你确认，系统无法自动验证资金性质。")}
          </p>
          <label className="checkbox">
            <input
              type="checkbox"
              checked={environmentConfirmed}
              disabled={environment === "unknown"}
              onChange={e => setEnvironmentConfirmed(e.target.checked)}
            />
            {t("我已核对账户与柜台环境")}
          </label>
        </section>
      )}
      {environmentReady && (
        <p className={environment === "real" ? "alert" : "subtle"}>
          {t(
            environment === "real"
              ? "真实资金账户：授权后的委托可能产生真实成交与盈亏。"
              : "已标记为 CTP 仿真；委托仍会发送到上方柜台，请确认地址正确。",
          )}
        </p>
      )}
      <details className="lifecycle-help">
        <summary>{t("连接说明")}</summary>
        <p>{t("离开账户只断开 Terminal；断开账户会退出柜台连接，已有委托不会自动撤销。")}</p>
      </details>
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
          <fieldset disabled={busy || stale || !environmentReady}>
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
        <section className="trading-permission" aria-label={t("交易授权")}>
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
                {t("我确认使用账户 {user} 向上方柜台发送委托", { user: live.broker.user_id })}
              </label>
              <div className="source-actions">
                <button
                  className="primary"
                  disabled={busy || !confirmed || stale || !environmentReady}
                  onClick={() => {
                    setConfirmed(false);
                    void act({ action: "live_authorize", user_id: live.broker.user_id });
                  }}
                >
                  {t("允许发送委托")}
                </button>
                <span className="subtle">{t("授权只在本次连接和当前交易日有效。")}</span>
              </div>
            </>
          )}
        </section>
      )}
      {ready && (
        <details className="account-rates-details">
          <summary>{t("账户费率与模板")}</summary>
          <AccountRates live={live} busy={busy} run={run} />
        </details>
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
      <form
        aria-label={t("CTP 委托")}
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
        <fieldset disabled={busy || !ready || !authorized || stale || !environmentReady}>
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
              {t("提交柜台委托")}
            </button>
            <span className="subtle">
              {t("委托先写入本机交易记录再发送；断线后不会自动重发。")}
            </span>
          </div>
        </fieldset>
      </form>
      <ActivityTabs value={activity} onChange={setActivity} />
      <div hidden={activity !== "positions"}>
        <h3 className="paper-heading">{t("持仓")}</h3>
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
        </div>
      </div>
      <div hidden={activity !== "orders"}>
        <h3 className="paper-heading">{t("委托")}</h3>
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
      </div>
      <div hidden={activity !== "fills"}>
        <h3 className="paper-heading">{t("成交")}</h3>
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
        </div>
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
