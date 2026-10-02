import { AccountLibrary, accountName } from "../trading/AccountLibrary";
import { ActivityTabs, type Activity } from "../trading/ActivityTabs";
import { FlowSteps } from "../../src/ui/FlowSteps";
import {
  ContractCosts,
  CostScheduleDetails,
  contractCostRequest,
  type ContractCostDrafts,
  DatasetPicker,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
import { useWorkspaceDraft, translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import { StrategyPanel } from "./StrategyPanel";
import { useEffect, useRef, useState } from "react";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import { timestamp, type TerminalCommand } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
export function ReplayPanel(context: TerminalContext) {
  const { snapshot, busy, trade, navigate } = context;
  const [creating, setCreating] = useWorkspaceDraft("paper-creating", false);
  const [step, setStep] = useWorkspaceDraft("paper-step", 0);
  const [name, setName] = useWorkspaceDraft("paper-name", "");
  const [activity, setActivity] = useState<Activity>("positions");
  const states: Record<string, string | number> = {
    accepted: t("待成交"),
    partially_filled: t("部分成交"),
    filled: t("已成交"),
    cancelled: t("已撤单"),
  };
  const offsets: Record<string, string | number> = {
    open: t("开仓"),
    close_today: t("平今"),
    close_yesterday: t("平昨"),
    close: t("平仓"),
  };
  const paper = snapshot?.paper;
  const remote = snapshot?.connection?.transport === "tcp_tls";
  const [directory, setDirectory] = useWorkspaceDraft("replay-directory", "");
  const [account, setAccount] = useWorkspaceDraft("replay-account", {
    deposit: "",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [costs, setCosts] = useWorkspaceDraft<ContractCostDrafts>("replay-contract-costs", {});
  const datasets = snapshot?.datasets ?? [];
  const [order, setOrder] = useState({
    contract: "",
    side: "buy",
    offset: "open",
    quantity: "1",
    price: "",
  });
  // The order's contract: the chosen one, else the first of the portfolio.
  const traded =
    paper?.contracts.find(
      item => `${item.contract.venue}.${item.contract.symbol}` === order.contract,
    ) ?? paper?.contracts[0];
  const currency = paper?.contracts[0]?.contract.currency ?? "";
  // Mirrors the core's ClosePolicy: only SHFE/INE (and unverified venues)
  // take explicit today/yesterday closes; the rest assign buckets themselves.
  const venue = traded?.contract.venue ?? "";
  const explicitBuckets = !["CFFEX", "DCE", "CZCE", "GFEX"].includes(venue);
  const offsetValue =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
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
  function act(params: Record<string, string | number>) {
    return run("paper.act", { request_id: crypto.randomUUID(), ...params });
  }
  const blocked =
    busy ||
    !!snapshot?.stale ||
    snapshot?.connection?.state === "disconnected" ||
    paper?.storage_state === "recovery_required" ||
    !!paper?.strategy?.active;
  // Automatic settlement sends the same command as the button, once per day.
  const [autoSettle, setAutoSettle] = useWorkspaceDraft("replay-auto-settle", false);
  const settledDay = useRef<number | null>(null);
  const due = paper?.replay?.settlement_due ? paper.replay.settled_days : null;
  function settle() {
    if (due === null) return;
    settledDay.current = due;
    void act({ action: "replay_settle", day_index: due });
  }
  useEffect(() => {
    if (autoSettle && due !== null && !blocked && settledDay.current !== due) settle();
  });
  const owner = snapshot?.nodes
    .find(n => n.id === "local")
    ?.health?.services.find(service => service.id === snapshot?.connection?.session);
  async function createAccount() {
    const request = { ...account, contracts: contractCostRequest(datasets, costs) };
    if (
      await run(
        "paper.create",
        remote ? request : { ...(directory ? { directory } : { name }), ...request },
      )
    ) {
      setCreating(false);
      setStep(0);
    }
  }
  return (
    <section className="futures-data paper-trading" aria-label={t("期货模拟交易")}>
      <div className="panel-heading">
        <h2>
          {paper
            ? accountName(owner?.directory ?? "", snapshot?.connection?.session ?? t("历史回放"))
            : t("历史回放")}
        </h2>
        <span className="panel-spacer" />
        <small>{t("历史回放 · 非实盘")}</small>
      </div>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.research" />
        </p>
      )}
      {!paper && !creating && !remote ? (
        <AccountLibrary
          context={context}
          kind="paper"
          directory={directory}
          setDirectory={setDirectory}
          onCreate={() => {
            setCreating(true);
            setStep(0);
          }}
          onOpen={value => void run("paper.open", { directory: value })}
          onError={reason => setError(asDisplayError(reason))}
        />
      ) : !paper ? (
        <div className="account-setup">
          <div className="workflow-heading">
            <h3>{t("新建回放账户")}</h3>
            <button disabled={busy} onClick={() => setCreating(false)}>
              {t("取消")}
            </button>
          </div>
          <FlowSteps labels={[t("选择历史数据"), t("账户与规则"), t("确认创建")]} current={step} />
          <div hidden={step !== 0}>
            <p className="content-caption">
              {t(
                "选择从数据源下载的一个或多个合约的 K 线创建模拟账户，组合共用一个资金账户。会话写入交易服务所在机器的专用目录，重启后可恢复。",
              )}
            </p>
            <DatasetPicker query={context.query} snapshot={snapshot} busy={busy} trade={trade} />
            <div className="workflow-actions">
              <button onClick={() => navigate("workspace.data")}>{t("下载历史数据")}</button>
              <button
                className="primary"
                disabled={busy || !datasets.length}
                onClick={() => setStep(1)}
              >
                {t("下一步")}
              </button>
            </div>
          </div>
          <form
            hidden={step !== 1}
            onSubmit={e => {
              e.preventDefault();
              setStep(2);
            }}
          >
            <fieldset
              disabled={busy || step !== 1 || snapshot?.connection?.state === "disconnected"}
            >
              {!remote && (
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
              )}
              {!remote && (
                <details>
                  <summary>{t("自定义记录目录")}</summary>
                  <p className="subtle">{t("留空时自动创建专用目录，之后从账户列表打开。")}</p>
                  <div className="futures-file">
                    <label>
                      {t("交易记录目录")}
                      <input
                        aria-label={t("交易记录目录")}
                        value={directory}
                        onChange={e => setDirectory(e.target.value)}
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
              )}
              {remote && (
                <p>
                  {t("远程会话：")} {snapshot?.connection?.session} · {snapshot?.connection?.host}
                  {t("。记录目录由服务端管理。")}
                </p>
              )}
              {[
                {
                  title: t("账户"),
                  fields: [["deposit", t("初始模拟资金")]],
                },
                {
                  title: t("委托与持仓限制"),
                  fields: [
                    ["max_order_quantity", t("单笔数量上限")],
                    ["max_gross_quantity", t("总持仓量上限")],
                    ["max_working_orders", t("在途委托数上限")],
                  ],
                },
              ].map(group => (
                <section className="account-field-group" key={group.title} aria-label={group.title}>
                  <h3>{group.title}</h3>
                  <div className="futures-fields">
                    {group.fields.map(([key, label]) => (
                      <label key={key}>
                        {label}
                        <input
                          aria-label={label}
                          inputMode="decimal"
                          value={account[key as keyof typeof account]}
                          onChange={e => setAccount({ ...account, [key]: e.target.value })}
                          required
                        />
                      </label>
                    ))}
                  </div>
                </section>
              ))}
              <ContractCosts datasets={datasets} drafts={costs} onChange={setCosts} />
              <div className="workflow-actions">
                <button type="button" onClick={() => setStep(0)}>
                  {t("上一步")}
                </button>
                <button className="primary" type="submit" disabled={!datasets.length}>
                  {t("下一步")}
                </button>
              </div>
            </fieldset>
          </form>
          {step === 2 && (
            <section className="creation-review">
              <h3>{name || snapshot?.connection?.session || t("历史回放")}</h3>
              <p>{datasets.map(item => `${item.venue} · ${item.symbol}`).join(" + ")}</p>
              <p>
                {t("初始模拟资金")}：{account.deposit}
              </p>
              <p>
                {t("单笔数量上限")}：{account.max_order_quantity} · {t("总持仓量上限")}：
                {account.max_gross_quantity} · {t("在途委托数上限")}：{account.max_working_orders}
              </p>
              <p className="subtle">{t("历史回放 · 无真实委托")}</p>
              <p className="content-caption">
                {t(
                  "保证金与手续费由你填写，不代表交易所规则。委托在下一根 K 线以保守价格撮合，每根最多成交该 K 线成交量的 10%；每个交易日结束按日线结算价结算。",
                )}
              </p>
              <div className="workflow-actions">
                <button disabled={busy} onClick={() => setStep(1)}>
                  {t("上一步")}
                </button>
                <button
                  className="primary"
                  disabled={busy || !datasets.length}
                  onClick={() => void createAccount()}
                >
                  {t("创建回放账户")}
                </button>
              </div>
            </section>
          )}
        </div>
      ) : (
        <>
          <div className="paper-toolbar">
            <strong>
              {paper.contracts
                .map(item => `${item.contract.venue} · ${item.contract.symbol}`)
                .join(" + ")}
            </strong>
            <span>{t("{cursor} / {total} 根", { cursor: paper.cursor, total: paper.total })}</span>
            <span>{timestamp(paper.timestamp_ns)}</span>
            {paper.replay && (
              <span>
                {t("已结算 {settled} / {days} 个交易日", {
                  settled: paper.replay.settled_days,
                  days: paper.replay.days,
                })}
              </span>
            )}
            <button
              disabled={blocked || paper.cursor === paper.total || paper.replay?.settlement_due}
              onClick={() => void act({ action: "advance" })}
            >
              {t("回放下一根")}
            </button>
            {paper.replay?.settlement_due && (
              <button className="primary" disabled={blocked} onClick={settle}>
                {t("日终结算")}
              </button>
            )}
            <label className="checkbox">
              <input
                type="checkbox"
                checked={autoSettle}
                onChange={event => setAutoSettle(event.target.checked)}
              />
              {t("自动日终结算")}
            </label>
            <span className="panel-spacer" />
            <button disabled={busy} onClick={() => void run("paper.close")}>
              {t("断开连接")}
            </button>
          </div>
          {paper.storage_state === "recovery_required" && (
            <p className="alert">
              {remote
                ? t(
                    "交易连接或存储状态不确定。请在设置 → 连接中重新连接并核对结果；断线命令不会自动重发。",
                  )
                : t("交易连接或存储状态不确定，请关闭会话并从原目录恢复，核对结果后再操作。")}
            </p>
          )}
          <p className="subtle account-location">
            {remote
              ? `${snapshot?.connection?.host} · ${snapshot?.connection?.session}`
              : t("本机")}{" "}
            · {t("断开仅离开账户，服务与已有任务继续运行。")}
          </p>
          {paper.cursor === paper.total && (
            <p role="status">{t("历史回放已结束，可查看记录或创建新的账户。")}</p>
          )}
          <dl className="paper-metrics">
            {(
              [
                ["equity", t("权益")],
                ["available", t("可用资金")],
                ["balance", t("账面资金")],
                ["margin", t("占用保证金")],
                ["frozen", t("委托冻结")],
                ["unrealized", t("浮动盈亏")],
                ["realized", t("已实现盈亏")],
                ["fees", t("累计手续费")],
              ] as const
            ).map(([key, label]) => (
              <div key={key}>
                <dt>{label}</dt>
                <dd data-testid={`paper-${key}`}>
                  {paper[key]} <small>{currency}</small>
                </dd>
              </div>
            ))}
          </dl>
          <form
            onSubmit={e => {
              e.preventDefault();
              const { contract: _chosen, ...fields } = order;
              void act({
                action: "submit",
                order_id: crypto.randomUUID(),
                venue: traded?.contract.venue ?? "",
                symbol: traded?.contract.symbol ?? "",
                ...fields,
                offset: offsetValue,
              });
            }}
          >
            <fieldset
              disabled={
                blocked || !paper.cursor || paper.cursor === paper.total || paper.replay?.day_end
              }
            >
              <div className="futures-fields">
                <label>
                  {t("合约")}
                  <select
                    aria-label={t("委托合约")}
                    value={traded ? `${traded.contract.venue}.${traded.contract.symbol}` : ""}
                    onChange={e => setOrder({ ...order, contract: e.target.value })}
                  >
                    {paper.contracts.map(item => (
                      <option
                        key={`${item.contract.venue}.${item.contract.symbol}`}
                        value={`${item.contract.venue}.${item.contract.symbol}`}
                      >
                        {item.contract.venue} · {item.contract.symbol}
                      </option>
                    ))}
                  </select>
                </label>
                <label>
                  {t("买卖方向")}
                  <select
                    aria-label={t("买卖方向")}
                    value={order.side}
                    onChange={e => setOrder({ ...order, side: e.target.value })}
                  >
                    <option value="buy">{t("买入")}</option>
                    <option value="sell">{t("卖出")}</option>
                  </select>
                </label>
                <label>
                  {t("开平仓")}
                  <select
                    aria-label={t("开平仓")}
                    value={offsetValue}
                    onChange={e => setOrder({ ...order, offset: e.target.value })}
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
                    onChange={e => setOrder({ ...order, quantity: e.target.value })}
                    required
                  />
                </label>
                <label>
                  {t("限价")}
                  <input
                    aria-label={t("限价")}
                    value={order.price}
                    onChange={e => setOrder({ ...order, price: e.target.value })}
                    placeholder={traded?.mark}
                    required
                  />
                </label>
              </div>
              <div className="source-actions">
                <button className="primary" type="submit">
                  {t("提交模拟委托")}
                </button>
                <span className="subtle">{t("下单后从下一根 K 线开始撮合")}</span>
              </div>
            </fieldset>
          </form>
          <ActivityTabs value={activity} onChange={setActivity} strategy />
          <div hidden={activity !== "positions"}>
            <h3 className="paper-heading">{t("持仓")}</h3>
            <div className="paper-table" role="region" aria-label={t("模拟持仓")} tabIndex={0}>
              <table aria-label={t("模拟持仓")}>
                <thead>
                  <tr>
                    <th>{t("合约")}</th>
                    <th>{t("方向")}</th>
                    <th>{t("今昨仓")}</th>
                    <th>{t("手数")}</th>
                    <th>{t("计价成本")}</th>
                  </tr>
                </thead>
                <tbody>
                  {paper.positions.map((p, i) => (
                    <tr key={i}>
                      <td>{p.symbol}</td>
                      <td>{p.side === "buy" ? t("多头") : t("空头")}</td>
                      <td>{p.bucket === "today" ? t("今仓") : t("昨仓")}</td>
                      <td>{p.quantity}</td>
                      <td>{p.basis}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            {!paper.positions.length && <p className="content-caption">{t("暂无持仓")}</p>}
          </div>
          <div hidden={activity !== "orders"}>
            <h3 className="paper-heading">{t("委托")}</h3>
            <div className="paper-table" role="region" aria-label={t("模拟委托")} tabIndex={0}>
              <table aria-label={t("模拟委托")}>
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
                  {paper.orders
                    .slice()
                    .reverse()
                    .map(o => (
                      <tr key={o.id}>
                        <td>{o.symbol}</td>
                        <td>
                          {o.side === "buy" ? t("买入") : t("卖出")} / {offsets[o.offset]}
                        </td>
                        <td>{o.limit_price}</td>
                        <td>{o.quantity}</td>
                        <td>{o.filled}</td>
                        <td>{states[o.state] ?? o.state}</td>
                        <td>
                          {["accepted", "partially_filled"].includes(o.state) && (
                            <button
                              disabled={blocked}
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
            <div className="paper-table" role="region" aria-label={t("模拟成交")} tabIndex={0}>
              <table aria-label={t("模拟成交")}>
                <thead>
                  <tr>
                    <th>{t("合约")}</th>
                    <th>{t("成交编号")}</th>
                    <th>{t("价格")}</th>
                    <th>{t("手数")}</th>
                  </tr>
                </thead>
                <tbody>
                  {paper.fills
                    .slice()
                    .reverse()
                    .map(f => (
                      <tr key={f.id}>
                        <td>{f.symbol}</td>
                        <td>{f.id}</td>
                        <td>{f.price}</td>
                        <td>{f.quantity}</td>
                      </tr>
                    ))}
                </tbody>
              </table>
            </div>
          </div>
          {activity === "strategy" && (
            <StrategyPanel
              key={snapshot?.connection?.session}
              snapshot={snapshot}
              busy={busy}
              trade={trade}
            />
          )}
          <details>
            <summary>{t("风险限制")}</summary>
            <p>
              {t("单笔数量上限")}: {paper.risk.max_order_quantity} · {t("总持仓量上限")}:{" "}
              {paper.risk.max_gross_quantity} · {t("在途委托数上限")}:{" "}
              {paper.risk.max_working_orders}
            </p>
          </details>
          <details className="futures-help">
            <summary>{t("模拟规则与日终结算")}</summary>
            <p>
              {t(
                "委托在本合约的下一根 K 线撮合：买单在最低价不高于限价时按开盘价与限价中较低者成交，卖单对称；每根最多成交该 K 线成交量的 10%，按委托先后分配。无盘口、滑点或强平模型。浮亏会减少可用资金，浮盈不增加可开仓资金。存储保留数据集与操作日志。",
              )}
            </p>
            {paper.contracts.map(item => (
              <section key={`${item.contract.venue}.${item.contract.symbol}`}>
                <p>
                  {item.contract.venue} · {item.contract.symbol}
                  {t("：每手保证金")} {item.costs.margin_per_lot}
                  {t("；开仓 / 平今 / 平昨手续费")} {item.costs.open_fee} /{" "}
                  {item.costs.close_today_fee} / {item.costs.close_yesterday_fee}
                  {t("；保证金率")} {item.costs.margin_rate}
                  {t("；开仓 / 平今 / 平昨费率")} {item.costs.open_fee_rate} /{" "}
                  {item.costs.close_today_fee_rate} / {item.costs.close_yesterday_fee_rate}
                  {t("。单位：")} {item.contract.currency}。
                </p>
                <CostScheduleDetails versions={item.cost_schedule} />
              </section>
            ))}
            <p>
              {t(
                "结算价来自数据源日线；每个交易日所有合约回放完成后一起结算，再进入下一交易日。勾选自动日终结算后，每个交易日结束时自动发出同样的结算命令。",
              )}
            </p>
          </details>
        </>
      )}
    </section>
  );
}
