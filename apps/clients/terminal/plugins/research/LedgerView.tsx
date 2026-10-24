import { useState } from "react";
import type { EvaluationLedger, EvaluationLedgerCheck } from "../../src/bridge/client";
import { t } from "./shared";
const labels: Record<EvaluationLedgerCheck["metric"], string> = {
  settlement: "原结算价",
  position: "净持仓量",
  fees: "累计手续费",
  balance: "余额",
  equity: "权益",
  realized: "累计结算毛变动",
  unrealized: "账上未结算估值",
  frozen: "冻结资金",
  margin: "保证金",
  available: "可用资金",
};
export function LedgerChecks({
  rows,
  label,
  currency,
}: {
  rows: EvaluationLedgerCheck[];
  label: string;
  currency: string;
}) {
  return (
    <table aria-label={label}>
      <thead>
        <tr>
          {["核对项目", "单位", "桥接预期值", "原记录值", "记录减预期差额"].map(text => (
            <th key={text}>{t(text)}</th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map(row => (
          <tr key={row.metric}>
            <td>{t(labels[row.metric])}</td>
            <td>
              {row.metric === "position"
                ? t("手")
                : row.metric === "settlement"
                  ? t("原报价单位")
                  : currency}
            </td>
            <td>{row.expected}</td>
            <td>{row.recorded}</td>
            <td>{row.residual}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
export function LedgerView({ ledger, currency }: { ledger: EvaluationLedger; currency: string }) {
  const [page, setPage] = useState(0),
    [selected, setSelected] = useState<number | null>(null),
    [fillPage, setFillPage] = useState(0);
  const rows = ledger.days.slice(page * 50, page * 50 + 50);
  const day = selected === null ? null : ledger.days.find(row => row.source_index === selected);
  return (
    <section aria-label={t("固定原账本归因桥接")}>
      <h5>{t("固定原账本归因桥接")}</h5>
      <p>
        {ledger.venue}/{ledger.symbol} · {t("合约乘数")} {ledger.multiplier} · {currency}
      </p>
      <p role="status">
        {t(
          ledger.reconciled
            ? "账本金额核对一致，不代表策略有效或已获交易许可"
            : "存在未解释差额，不能采信完整归因",
        )}
      </p>
      <p>
        {t("日结盯市毛变动包括未平仓结算影响，不是已平仓交易收益；净手数为零不代表总持仓为零。")}
      </p>
      <dl>
        {(
          [
            ["原期初资金", ledger.deposit],
            ["累计结算毛变动", ledger.gross],
            ["累计手续费", ledger.fees],
            ["解释净变动", ledger.explained_net],
            ["原账户净变动", ledger.recorded_net],
            ["累计未解释差额", ledger.residual],
          ] as const
        ).map(([label, value]) => (
          <div key={label}>
            <dt>{t(label)}</dt>
            <dd>
              {value} {currency}
            </dd>
          </div>
        ))}
      </dl>
      <p>
        {t("期末原净手数")}：{ledger.closing_net_quantity} · {t("期末原总手数")}：
        {ledger.closing_gross_quantity}
      </p>
      <p>{t("账上未结算估值为零不表示全部平仓；日结已确认的持仓影响保留在毛变动中。")}</p>
      <table aria-label={t("原日结毛变动费用与累计差额")}>
        <thead>
          <tr>
            {[
              "原行索引",
              "交易日",
              "期初净手数",
              "净手数变动",
              "期末净手数",
              "日结毛变动",
              "当日手续费",
              "预期余额",
              "原余额",
              "原权益",
              "累计未解释差额",
              "原件明细",
            ].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(row => (
            <tr key={row.source_index}>
              <td>{row.source_index}</td>
              <td>{row.trading_day}</td>
              <td>{row.opening_quantity}</td>
              <td>{row.quantity_change}</td>
              <td>{row.closing_quantity}</td>
              <td>{row.gross}</td>
              <td>{row.fees}</td>
              <td>{row.expected_balance}</td>
              <td>{row.recorded_balance}</td>
              <td>{row.recorded_equity}</td>
              <td>{row.cumulative_residual}</td>
              <td>
                <button
                  onClick={() => {
                    setSelected(row.source_index);
                    setFillPage(0);
                  }}
                >
                  {t("查看原成交与核对项")}
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <p>
        {page * 50 + 1}–{page * 50 + rows.length} / {ledger.days.length}
      </p>
      <button disabled={page === 0} onClick={() => setPage(page - 1)}>
        {t("上一页")}
      </button>
      <button disabled={(page + 1) * 50 >= ledger.days.length} onClick={() => setPage(page + 1)}>
        {t("下一页")}
      </button>
      {day && (
        <section aria-label={t("所选原日结明细")}>
          <h6>
            {day.trading_day} · {day.source_index}
          </h6>
          <p>
            {t("原观察时点UTC")}：
            {day.timestamp_ns === "0"
              ? t("未记录观察时点")
              : new Date(Number(BigInt(day.timestamp_ns) / 1000000n)).toISOString()}{" "}
            <code>{day.timestamp_ns}</code>
          </p>
          <LedgerChecks rows={day.checks} label={t("所选日结核对项")} currency={currency} />
          <table aria-label={t("所选日结原成交")}>
            <thead>
              <tr>
                {["成交身份", "委托身份", "方向", "成交数量", "成交价格", "原手续费"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {day.fills.slice(fillPage * 50, fillPage * 50 + 50).map(fill => (
                <tr key={fill.id}>
                  <td>{fill.id}</td>
                  <td>{fill.order_id}</td>
                  <td>{t(fill.buy ? "买入" : "卖出")}</td>
                  <td>{fill.quantity}</td>
                  <td>{fill.price}</td>
                  <td>{fill.fee}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {!day.fills.length && <p>{t("该原交易日没有成交，不伪造交易或收益。")}</p>}
          <button disabled={fillPage === 0} onClick={() => setFillPage(fillPage - 1)}>
            {t("上一页成交")}
          </button>
          <button
            disabled={(fillPage + 1) * 50 >= day.fills.length}
            onClick={() => setFillPage(fillPage + 1)}
          >
            {t("下一页成交")}
          </button>
        </section>
      )}
      <LedgerChecks rows={ledger.final_checks} label={t("期末原账户核对项")} currency={currency} />
      <p>
        {t("本表只读已发布计算，不重算或修订原账本；方向、策略、多合约及交易多维归因仍未覆盖。")}
      </p>
    </section>
  );
}
