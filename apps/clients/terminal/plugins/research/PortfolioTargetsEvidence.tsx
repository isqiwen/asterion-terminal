import { useState } from "react";
import type { TaskResult } from "../../src/bridge/client";
import { t } from "./shared";

type Evidence = Extract<TaskResult, { kind: "backtest" }>;
export function PortfolioTargetsEvidence({ evidence }: { evidence: Evidence }) {
  const [page, setPage] = useState(0),
    [selected, setSelected] = useState<number | null>(0),
    [fillPage, setFillPage] = useState(0),
    [orderPage, setOrderPage] = useState(0),
    [holdingPage, setHoldingPage] = useState(0);
  const policy = evidence.experiment.strategies[0]?.rule;
  if (policy?.kind !== "fixed_weights") return null;
  const decisions = evidence.result.portfolio_targets ?? [];
  const current = selected === null ? null : decisions[selected];
  const originalFills = new Map(evidence.result.account.fills.map(f => [f.id, f]));
  const originalOrders = new Map(evidence.result.account.orders.map(o => [o.id, o]));
  const originalHoldings = current?.members.flatMap(m => m.observation.positions) ?? [];
  const acceptedOrders = current
    ? (evidence.result.portfolio_orders ?? []).filter(o =>
        current.members.some(m => m.intent_id === o.intent_id),
      )
    : [];
  const associated = current
    ? (evidence.result.portfolio_fills ?? []).filter(f =>
        current.members.some(m => m.intent_id === f.intent_id),
      )
    : [];
  return (
    <section aria-label={t("固定组合目标与实际执行")}>
      <h4>{t("固定组合目标与实际执行")}</h4>
      <p>{t("只读原计算的共同边界权益、目标和实际成交关联；不在页面重算手数、收益或费用。")}</p>
      <p>
        {t(
          "名义余量不是现金，不自动转配；实际持仓和在途与目标分列，资金或风控拒绝不等于改小了原目标。",
        )}
      </p>
      <p>
        {t(
          "原bar边界不证明资料当时已可得；本结果仍是固定资料的离线条件研究，不是独立验证或实盘授权。",
        )}
      </p>
      <p>
        {t("固定调仓间隔")}：{policy.rebalance} · {t("组合总绝对名义权重预算")}：
        {policy.gross_weight_limit} · {t("组合净名义权重预算")}：{policy.net_weight_limit}
      </p>
      {!decisions.length ? (
        <p role="status">{t("未到声明的共同调仓边界，没有生成组合目标。")}</p>
      ) : (
        <>
          <table aria-label={t("全部原组合调仓边界")}>
            <thead>
              <tr>
                {[
                  "交易日",
                  "原共同边界ns",
                  "原边界权益",
                  "请求总绝对名义",
                  "请求净名义",
                  "整数目标总绝对名义",
                  "未分配名义预算",
                  "存在后续bar",
                  "原件明细",
                ].map(s => (
                  <th key={s}>{t(s)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {decisions.slice(page * 50, page * 50 + 50).map((d, i) => (
                <tr key={d.boundary_ns}>
                  <td>{d.trading_day}</td>
                  <td>
                    <code>{d.boundary_ns}</code>
                  </td>
                  <td>{d.equity}</td>
                  <td>{d.requested_gross_notional}</td>
                  <td>{d.requested_net_notional}</td>
                  <td>{d.allocated_gross_notional}</td>
                  <td>{d.unallocated_nominal_budget}</td>
                  <td>{t(d.has_next_bar ? "有后续bar，不保证成交" : "末边界，无后续bar成交")}</td>
                  <td>
                    <button
                      onClick={() => {
                        setSelected(page * 50 + i);
                        setFillPage(0);
                        setOrderPage(0);
                        setHoldingPage(0);
                      }}
                    >
                      {t("查看原组合目标与成交")}
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <p>
            {page * 50 + 1}–{Math.min((page + 1) * 50, decisions.length)} / {decisions.length}
          </p>
          <button
            disabled={page === 0}
            onClick={() => {
              setPage(page - 1);
              setSelected(null);
              setFillPage(0);
              setOrderPage(0);
              setHoldingPage(0);
            }}
          >
            {t("上一页组合边界")}
          </button>
          <button
            disabled={(page + 1) * 50 >= decisions.length}
            onClick={() => {
              setPage(page + 1);
              setSelected(null);
              setFillPage(0);
              setOrderPage(0);
              setHoldingPage(0);
            }}
          >
            {t("下一页组合边界")}
          </button>
        </>
      )}
      {current && (
        <section aria-label={t("所选原组合目标与成交")}>
          <h5>
            {current.trading_day} · <code>{current.boundary_ns}</code>
          </h5>
          <p>
            {t("原观察时点UTC")}：
            {new Date(Number(BigInt(current.boundary_ns) / 1000000n)).toISOString()}
          </p>
          <p>
            {t("原边界权益")}：{current.equity} · {t("币种")}：
            {evidence.result.account.contracts[0].contract.currency}
          </p>
          <table aria-label={t("所选原整数目标与余量")}>
            <thead>
              <tr>
                {[
                  "真实合约",
                  "原行索引",
                  "原目标意图",
                  "原名义权重",
                  "原收盘价",
                  "合约乘数",
                  "原目标名义",
                  "原整数目标手数",
                  "原整数名义",
                  "未使用名义余量",
                ].map(s => (
                  <th key={s}>{t(s)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {current.members.map(m => (
                <tr key={m.intent_id}>
                  <td>
                    {m.venue}/{m.symbol}
                  </td>
                  <td>{m.source_index}</td>
                  <td>
                    <code>{m.intent_id}</code>
                  </td>
                  <td>{m.weight}</td>
                  <td>{m.price}</td>
                  <td>{m.multiplier}</td>
                  <td>{m.requested_notional}</td>
                  <td>{m.quantity}</td>
                  <td>{m.allocated_notional}</td>
                  <td>{m.nominal_remainder}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p>
            {t("委托状态取自原回测终点，不冒充决策边界的在途；日单余量到期不代表原目标已完成。")}
          </p>
          <p>
            {t(
              "原持仓捕获于共同bar成交及日单到期处理后、新目标接纳前；保留真实今昨多空批次，不以终点仓位回填。",
            )}
          </p>
          <table aria-label={t("原调仓边界实际持仓批次")}>
            <thead>
              <tr>
                {["原合约", "原持仓方向", "原持仓今昨", "原数量", "原持仓成本基准"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {originalHoldings.slice(holdingPage * 50, holdingPage * 50 + 50).map((lot, i) => (
                <tr key={holdingPage * 50 + i}>
                  <td>
                    {lot.venue}/{lot.symbol}
                  </td>
                  <td>{t(lot.side === "buy" ? "多头" : "空头")}</td>
                  <td>{t(lot.bucket === "today" ? "今仓" : "昨仓")}</td>
                  <td>{lot.quantity}</td>
                  <td>{lot.basis}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {!originalHoldings.length && <p>{t("原边界持仓已知为空，不代表原目标已完成。")}</p>}
          <button disabled={holdingPage === 0} onClick={() => setHoldingPage(holdingPage - 1)}>
            {t("上一页原持仓")}
          </button>
          <button
            disabled={(holdingPage + 1) * 50 >= originalHoldings.length}
            onClick={() => setHoldingPage(holdingPage + 1)}
          >
            {t("下一页原持仓")}
          </button>
          <p>{t("当前日线截面已处理所属日单到期，原工作委托已知为空；新目标仍不是已接纳委托。")}</p>
          <table aria-label={t("原组合意图的实际接纳委托")}>
            <thead>
              <tr>
                {[
                  "原目标意图",
                  "原决策行",
                  "实际接纳行",
                  "实际bar边界ns",
                  "委托身份",
                  "原委托方向",
                  "原委托开平",
                  "原委托数量",
                  "原累计成交",
                  "终点原委托状态",
                ].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {acceptedOrders.slice(orderPage * 50, orderPage * 50 + 50).map(a => {
                const o = originalOrders.get(a.order_id)!;
                const state: Record<string, string> = {
                  accepted: "已接纳",
                  partially_filled: "部分成交",
                  filled: "全部成交",
                  cancelled: "已撤余量",
                  rejected: "已拒绝",
                };
                const offset: Record<string, string> = {
                  open: "开仓",
                  close: "平仓",
                  close_today: "平今",
                  close_yesterday: "平昨",
                };
                return (
                  <tr key={a.order_id}>
                    <td>
                      <code>{a.intent_id}</code>
                    </td>
                    <td>{a.decision_source_index}</td>
                    <td>{a.source_index}</td>
                    <td>
                      <code>{a.boundary_ns}</code>
                    </td>
                    <td>
                      <code>{a.order_id}</code>
                    </td>
                    <td>{t(o.side === "buy" ? "买入" : "卖出")}</td>
                    <td>{t(offset[o.offset] ?? o.offset)}</td>
                    <td>{o.quantity}</td>
                    <td>{o.filled}</td>
                    <td>{t(state[o.state] ?? o.state)}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {!acceptedOrders.length && <p>{t("本轮未记录实际接纳委托，不将目标当作已发送订单。")}</p>}
          <button disabled={orderPage === 0} onClick={() => setOrderPage(orderPage - 1)}>
            {t("上一页原委托")}
          </button>
          <button
            disabled={(orderPage + 1) * 50 >= acceptedOrders.length}
            onClick={() => setOrderPage(orderPage + 1)}
          >
            {t("下一页原委托")}
          </button>
          <table aria-label={t("原组合意图的实际成交")}>
            <thead>
              <tr>
                {[
                  "原目标意图",
                  "原决策行",
                  "实际成交行",
                  "实际bar边界ns",
                  "成交身份",
                  "委托身份",
                  "原数量",
                  "原成交价",
                  "原手续费",
                ].map(s => (
                  <th key={s}>{t(s)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {associated.slice(fillPage * 50, fillPage * 50 + 50).map(a => {
                const f = originalFills.get(a.fill_id)!;
                return (
                  <tr key={a.fill_id}>
                    <td>
                      <code>{a.intent_id}</code>
                    </td>
                    <td>{a.decision_source_index}</td>
                    <td>{a.source_index}</td>
                    <td>
                      <code>{a.boundary_ns}</code>
                    </td>
                    <td>
                      <code>{f.id}</code>
                    </td>
                    <td>
                      <code>{f.order_id}</code>
                    </td>
                    <td>{f.quantity}</td>
                    <td>{f.price}</td>
                    <td>{f.fee}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {!associated.length && <p>{t("本轮未记录实际成交，不将目标或委托当作成交。")}</p>}
          <button disabled={fillPage === 0} onClick={() => setFillPage(fillPage - 1)}>
            {t("上一页原成交")}
          </button>
          <button
            disabled={(fillPage + 1) * 50 >= associated.length}
            onClick={() => setFillPage(fillPage + 1)}
          >
            {t("下一页原成交")}
          </button>
        </section>
      )}
    </section>
  );
}
