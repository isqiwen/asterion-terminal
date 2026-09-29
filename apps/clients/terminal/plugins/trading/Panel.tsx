import { ErrorNotice, asDisplayError, type DisplayError } from "../contract";
import { useWorkspaceDraft, translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import { StrategyPanel } from "./StrategyPanel";
import { useState } from "react";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import { timestamp, type TerminalCommand } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
export function Panel({ snapshot, busy, trade, navigate }: TerminalContext) {
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
  const [costs, setCosts] = useWorkspaceDraft("costs", {
    deposit: "",
    margin_per_lot: "",
    open_fee: "",
    close_today_fee: "",
    close_yesterday_fee: "",
    margin_rate: "0",
    open_fee_rate: "0",
    close_today_fee_rate: "0",
    close_yesterday_fee_rate: "0",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [order, setOrder] = useState({ side: "buy", offset: "open", quantity: "1", price: "" });
  // Mirrors the core's ClosePolicy: only SHFE/INE (and unverified venues)
  // take explicit today/yesterday closes; the rest assign buckets themselves.
  const venue = paper?.contract.venue ?? snapshot?.dataset?.venue ?? "";
  const explicitBuckets = !["CFFEX", "DCE", "CZCE", "GFEX"].includes(venue);
  const offsetValue =
    order.offset === "open"
      ? "open"
      : explicitBuckets
        ? order.offset === "close"
          ? "close_today"
          : order.offset
        : "close";
  const [settlement, setSettlement] = useState("");
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
              "选择已发布的单合约成交数据创建模拟账户。会话写入交易服务所在机器的专用目录，重启后可恢复。",
            )}
          </p>
          <form
            onSubmit={e => {
              e.preventDefault();
              void run("paper.create", remote ? costs : { directory, ...costs });
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
              <p>
                {t("当前数据：")}{" "}
                {snapshot?.dataset
                  ? t("{p0} · {p1} 笔", { p0: snapshot.dataset.symbol, p1: snapshot.dataset.count })
                  : t("尚未选择")}{" "}
                <button
                  type="button"
                  onClick={() => navigate("workspace.data", { page: "history" })}
                >
                  {t("选择历史数据")}
                </button>
              </p>
              {[
                {
                  title: t("账户与保证金"),
                  fields: [
                    ["deposit", t("初始模拟资金")],
                    ["margin_per_lot", t("每手保证金")],
                    ["margin_rate", t("保证金率")],
                  ],
                },
                {
                  title: t("交易费用"),
                  fields: [
                    ["open_fee", t("每手开仓费")],
                    ["close_today_fee", t("每手平今费")],
                    ["close_yesterday_fee", t("每手平昨费")],
                    ["open_fee_rate", t("开仓费率")],
                    ["close_today_fee_rate", t("平今费率")],
                    ["close_yesterday_fee_rate", t("平昨费率")],
                  ],
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
                          value={costs[key as keyof typeof costs]}
                          onChange={e => setCosts({ ...costs, [key]: e.target.value })}
                          required
                        />
                      </label>
                    ))}
                  </div>
                </section>
              ))}
              <div className="source-actions">
                <button className="primary" type="submit" disabled={!snapshot?.dataset}>
                  {t("创建模拟会话")}
                </button>
              </div>
            </fieldset>
          </form>
          <p className="dashboard-caption">
            {t(
              "保证金与手续费由你填写，不代表交易所规则。最多 10000 笔，按一个交易日模拟，不自动识别夜盘或跨日。",
            )}
          </p>
        </>
      ) : (
        <>
          <div className="paper-toolbar">
            <strong>
              {paper.contract.venue} · {paper.contract.symbol}
            </strong>
            <span>{t("{cursor} / {total} 笔", { cursor: paper.cursor, total: paper.total })}</span>
            <span>{timestamp(paper.timestamp_ns)}</span>
            <button
              disabled={blocked || paper.cursor === paper.total || paper.replay?.settlement_due}
              onClick={() => void act({ action: "advance" })}
            >
              {t("回放下一笔")}
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
                  {paper[key]} <small>{paper.contract.currency}</small>
                </dd>
              </div>
            ))}
          </dl>
          <form
            onSubmit={e => {
              e.preventDefault();
              void act({
                action: "submit",
                order_id: crypto.randomUUID(),
                ...order,
                offset: offsetValue,
              });
            }}
          >
            <fieldset
              disabled={
                blocked ||
                !paper.cursor ||
                paper.cursor === paper.total ||
                paper.replay?.session_end
              }
            >
              <div className="futures-fields">
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
                    placeholder={paper.mark}
                    required
                  />
                </label>
              </div>
              <div className="source-actions">
                <button className="primary" type="submit">
                  {t("提交模拟委托")}
                </button>
                <span className="subtle">{t("下单后从下一笔行情开始撮合")}</span>
              </div>
            </fieldset>
          </form>
          <h3 className="paper-heading">{t("持仓")}</h3>
          <div className="paper-table" role="region" aria-label={t("模拟持仓")} tabIndex={0}>
            <table aria-label={t("模拟持仓")}>
              <thead>
                <tr>
                  <th>{t("方向")}</th>
                  <th>{t("今昨仓")}</th>
                  <th>{t("手数")}</th>
                  <th>{t("计价成本")}</th>
                </tr>
              </thead>
              <tbody>
                {paper.positions.map((p, i) => (
                  <tr key={i}>
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
            <summary>{t("模拟规则与手动结算")}</summary>
            <p>
              {t(
                "成交按委托先后共享下一笔行情的成交量。价格穿过限价时直接成交；恰好触及限价时，需先消耗下单时该价位已成交的排队量。无盘口、滑点或强平模型。浮亏会减少可用资金，浮盈不增加可开仓资金。存储保留原始回放数据与操作日志。",
              )}
            </p>
            <p>
              {t("每手保证金")} {paper.costs.margin_per_lot}
              {t("；开仓 / 平今 / 平昨手续费")} {paper.costs.open_fee} /{" "}
              {paper.costs.close_today_fee} / {paper.costs.close_yesterday_fee}
              {t("；保证金率")} {paper.costs.margin_rate}
              {t("；开仓 / 平今 / 平昨费率")} {paper.costs.open_fee_rate} /{" "}
              {paper.costs.close_today_fee_rate} / {paper.costs.close_yesterday_fee_rate}
              {t("。单位：")} {paper.contract.currency}。
            </p>
            {paper.replay ? (
              <>
                <p>{t("结算价由绑定日程确定；每日完成后结算，再进入下一交易日。")}</p>
                <button
                  disabled={blocked || !paper.replay.settlement_due}
                  onClick={() =>
                    void act({ action: "replay_settle", day_index: paper.replay!.settled_days })
                  }
                >
                  {t("按日程结算")}
                </button>
              </>
            ) : (
              <>
                <p>
                  {t(
                    "回放结束并撤销剩余委托后，可输入结算价将浮盈亏计入资金、今仓转为昨仓。本版不自动跨日，也不支持在同一会话续接下一交易日。",
                  )}
                </p>
                <label>
                  {t("结算价")}
                  <input
                    aria-label={t("结算价")}
                    value={settlement}
                    onChange={e => setSettlement(e.target.value)}
                  />
                </label>
                <button
                  disabled={blocked || paper.cursor !== paper.total || !settlement}
                  onClick={() => void act({ action: "settle", price: settlement })}
                >
                  {t("手动结算")}
                </button>
              </>
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
