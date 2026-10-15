import { useState } from "react";
import type { AccountCheck, BacktestReconciliation } from "../../src/bridge/client";
import { t } from "./shared";
const metrics: Record<string, string> = {
  settlement: "结算价",
  position: "净持仓手数",
  fees: "累计手续费",
  balance: "资金余额",
  equity: "权益",
  realized: "累计已结算毛盈亏",
  unrealized: "未实现盈亏",
  frozen: "冻结资金",
  margin: "模型保证金",
  available: "可用资金",
};
function Checks({ checks }: { checks: AccountCheck[] }) {
  return (
    <table aria-label={t("资金持仓差异")}>
      <thead>
        <tr>
          {["核对项", "推算值", "记录值", "差额（记录减推算）"].map(key => (
            <th key={key}>{t(key)}</th>
          ))}
        </tr>
      </thead>
      <tbody>
        {checks.map(check => (
          <tr key={check.metric}>
            <td>{t(metrics[check.metric])}</td>
            <td>{check.expected}</td>
            <td>{check.recorded}</td>
            <td>
              {check.difference} · {t(check.matches ? "一致" : "有差异")}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
export function AccountReconciliation({ audit }: { audit: BacktestReconciliation }) {
  const [page, setPage] = useState(0);
  const [selectedDay, setSelectedDay] = useState("");
  if (audit.state !== "available")
    return (
      <details>
        <summary>{t("成交与资金核对")}</summary>
        <p>
          {t(
            audit.state === "missing_fill_days"
              ? "原结果缺少成交交易日证据，逐日核对未知；不推测日期，不自动重跑。"
              : "本核对限单合约日线结果；其他输入不据此判断一致或不一致。",
          )}
        </p>
      </details>
    );
  const model = audit.model!;
  const selected =
    audit.days.find(day => day.day === selectedDay) ??
    audit.days.find(day => day.fills.length) ??
    audit.days[0];
  const mismatch =
    audit.final_checks.filter(check => !check.matches).length +
    audit.days.reduce((count, day) => count + day.checks.filter(check => !check.matches).length, 0);
  const last = Math.max(0, Math.ceil(audit.days.length / 24) - 1),
    current = Math.min(page, last);
  return (
    <details>
      <summary>{t("成交与资金核对")}</summary>
      <p>
        {t(
          mismatch
            ? "存在差异：查看非零差额及对应交易日成交，核对固定输入、结算价和成本来源；保留原结果，不自动修复。"
            : "所列算术核对一致；这不证明历史参数真实，也不是柜台对账通过。",
        )}
      </p>
      <p className="subtle">
        {t(
          "仅使用原固定输入和保存结果。日结毛盈亏＝昨持仓×结算价变化×乘数＋当日各笔有符号成交手数×（结算价－成交价）×乘数；余额＝昨余额＋毛盈亏－手续费。买为正、卖为负，滑点已包含在成交价。每日结算重置持仓成本，累计已结算毛盈亏不等于已平仓轮次盈亏。",
        )}
      </p>
      <p>
        {model.venue}/{model.symbol} · {t("初始资金")} {model.deposit} · {t("固定乘数")}{" "}
        {model.multiplier}
      </p>
      <p>
        {t("期末净名义敞口")} {model.signed_notional} · {t("期末总名义敞口")} {model.gross_notional}
      </p>
      <p className="subtle">
        {t(
          "敞口按期末保存持仓手数×末次结算价×固定乘数计算，不是投入资金。保证金是资金占用，不从权益扣除；可用资金＝min（余额，权益）－保证金－冻结。",
        )}
      </p>
      <p>
        {t("固定成本来源")} {model.cost_source} · {t("生效起点")} {model.cost_effective_from} ·{" "}
        {t("每手保证金")} {model.margin_per_lot} · {t("名义保证金率")} {model.margin_rate}
      </p>
      <p className="subtle">
        {t(
          "模型保证金按保存的各持仓批次分别计算每手金额与名义费率部分，再相加，保留原模型舍入。历史保证金、乘数及规格适用性未在此证实；不查询当前值补齐，也不代表真实柜台要求。",
        )}
      </p>
      <div className="research-table">
        <Checks checks={audit.final_checks} />
      </div>
      <section aria-label={t("当日核对证据")}>
        <h4>
          {selected.day} · {t("当日核对证据")}
        </h4>
        <div className="research-table">
          <Checks checks={selected.checks} />
        </div>
        <div className="research-table">
          {" "}
          <table aria-label={t("当日成交证据")}>
            <thead>
              <tr>
                {["成交标识", "订单标识", "方向", "手数", "成交价", "手续费"].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {selected.fills.map(fill => (
                <tr key={fill.id}>
                  <td>{fill.id}</td>
                  <td>{fill.order_id}</td>
                  <td>{t(fill.side === "buy" ? "买" : "卖")}</td>
                  <td>{fill.quantity}</td>
                  <td>{fill.price}</td>
                  <td>{fill.fee}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>
      <div className="research-table">
        <table aria-label={t("逐日成交资金核对")}>
          <thead>
            <tr>
              {[
                "交易日",
                "期初手数",
                "净成交手数",
                "期末手数",
                "结算价",
                "期初余额",
                "毛盈亏",
                "当日手续费",
                "推算余额",
                "记录余额",
                "证据与差异",
              ].map(key => (
                <th key={key}>{t(key)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {audit.days.slice(current * 24, (current + 1) * 24).map(day => (
              <tr key={day.day}>
                {[
                  day.day,
                  day.opening_quantity,
                  day.quantity_change,
                  day.closing_quantity,
                  day.settlement,
                  day.opening_balance,
                  day.gross,
                  day.fees,
                  day.expected_balance,
                  day.recorded_balance,
                ].map((value, i) => (
                  <td key={i}>{value}</td>
                ))}
                <td>
                  <button
                    aria-pressed={selected.day === day.day}
                    onClick={() => setSelectedDay(day.day)}
                  >
                    {t(day.checks.every(check => check.matches) ? "一致" : "有差异")} ·{" "}
                    {day.fills.length}
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {last > 0 && (
        <div>
          <button disabled={current === 0} onClick={() => setPage(current - 1)}>
            {t("上一页")}
          </button>{" "}
          {current + 1}/{last + 1}{" "}
          <button disabled={current === last} onClick={() => setPage(current + 1)}>
            {t("下一页")}
          </button>
        </div>
      )}
    </details>
  );
}
