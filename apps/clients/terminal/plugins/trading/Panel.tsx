import {
  ContractCosts,
  contractCostRequest,
  type ContractCostDrafts,
  DatasetPicker,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
import { useWorkspaceDraft, translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import { StrategyPanel } from "./StrategyPanel";
import { useState } from "react";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import { timestamp, type TerminalCommand } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
export function Panel({ snapshot, busy, trade }: TerminalContext) {
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
  const [directory, setDirectory] = useWorkspaceDraft("directory", "");
  const [account, setAccount] = useWorkspaceDraft("account", {
    deposit: "",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [costs, setCosts] = useWorkspaceDraft<ContractCostDrafts>("contract-costs", {});
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
    } catch (reason) {
      setError(asDisplayError(reason));
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
  const blocked = busy || paper?.storage_state === "recovery_required" || !!paper?.strategy?.active;
  return (
    <section className="futures-data paper-trading" aria-label={t("期货模拟交易")}>
      <div className="panel-heading">
        <h2>{t("期货模拟交易")}</h2>
        <span className="panel-spacer" />
        <small>{t("历史回放 · 非实盘")}</small>
      </div>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {!paper ? (
        <>
          <p className="dashboard-caption">
            {t(
              "选择从数据源下载的一个或多个合约的 K 线创建模拟账户，组合共用一个资金账户。会话写入交易服务所在机器的专用目录，重启后可恢复。",
            )}
          </p>
          <DatasetPicker snapshot={snapshot} busy={busy} trade={trade} />
          <form
            onSubmit={e => {
              e.preventDefault();
              const request = { ...account, contracts: contractCostRequest(datasets, costs) };
              void run("paper.create", remote ? request : { directory, ...request });
            }}
          >
            <fieldset disabled={busy || snapshot?.connection?.state === "disconnected"}>
              {!remote && (
                <div className="futures-file">
                  <label>
                    {t("交易记录目录")}
                    <input
                      aria-label={t("交易记录目录")}
                      value={directory}
                      onChange={e => setDirectory(e.target.value)}
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
                    onClick={() => void run("paper.open", { directory })}
                  >
                    {t("恢复会话")}
                  </button>
                </div>
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
              <div className="source-actions">
                <button className="primary" type="submit" disabled={!datasets.length}>
                  {t("创建模拟会话")}
                </button>
              </div>
            </fieldset>
          </form>
          <p className="dashboard-caption">
            {t(
              "保证金与手续费由你填写，不代表交易所规则。委托在下一根 K 线以保守价格撮合，每根最多成交该 K 线成交量的 10%；每个交易日结束按日线结算价结算。",
            )}
          </p>
        </>
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
            <button
              disabled={blocked || paper.cursor === paper.total || paper.replay?.settlement_due}
              onClick={() => void act({ action: "advance" })}
            >
              {t("回放下一根")}
            </button>
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
          {!paper.positions.length && <p className="dashboard-caption">{t("暂无持仓")}</p>}
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
              <p key={`${item.contract.venue}.${item.contract.symbol}`}>
                {item.contract.venue} · {item.contract.symbol}
                {t("：每手保证金")} {item.costs.margin_per_lot}
                {t("；开仓 / 平今 / 平昨手续费")} {item.costs.open_fee} /{" "}
                {item.costs.close_today_fee} / {item.costs.close_yesterday_fee}
                {t("；保证金率")} {item.costs.margin_rate}
                {t("；开仓 / 平今 / 平昨费率")} {item.costs.open_fee_rate} /{" "}
                {item.costs.close_today_fee_rate} / {item.costs.close_yesterday_fee_rate}
                {t("。单位：")} {item.contract.currency}。
              </p>
            ))}
            <p>
              {t("结算价来自数据源日线；每个交易日所有合约回放完成后一起结算，再进入下一交易日。")}
            </p>
            {paper.replay && (
              <button
                disabled={blocked || !paper.replay.settlement_due}
                onClick={() =>
                  void act({ action: "replay_settle", day_index: paper.replay!.settled_days })
                }
              >
                {t("日终结算")}
              </button>
            )}
          </details>
        </>
      )}
      <StrategyPanel
        key={snapshot?.connection?.session ?? "no-account"}
        snapshot={snapshot}
        busy={busy}
        trade={trade}
      />
    </section>
  );
}
