import { useState } from "react";
import type { EvaluationExposure, EvaluationMetric } from "../../src/bridge/client";
import { percent, ratio } from "./Performance";
import { evaluationMetricReasons } from "./EvaluationMetricReasons";
import { t } from "./shared";
function observedRatio(value: EvaluationMetric, percentage = false) {
  return value.state === "AVAILABLE"
    ? percentage
      ? percent(value.value)
      : ratio(value.value)
    : t(evaluationMetricReasons[value.state]);
}
export function ExposureView({
  slice,
  equity,
  currency,
}: {
  slice: EvaluationExposure;
  equity: string;
  currency: string;
}) {
  const [page, setPage] = useState(0);
  const size = 50;
  const positions = slice.positions.slice(page * size, (page + 1) * size);
  const money = (value: string | null) => (value === null ? t("未记录") : `${value} ${currency}`);
  const quantity = (value: string | null) => (value === null ? t("未记录") : value);
  return (
    <section aria-label={t("原末日敞口与资金切片")}>
      <h4>{t("原末日敞口与资金切片")}</h4>
      <p>
        {t(
          "仅本固定范围末日结算列出的合约；净额不是全部风险，总额未记录时不从净手数推断。保证金占用不是最大亏损，也不证明可以退出。",
        )}
      </p>
      <p>
        {t("原结算索引")} {slice.source_index} · {slice.trading_day} · {t("原UTC纳秒")}{" "}
        {slice.timestamp_ns ?? t("未记录")}
      </p>
      <dl>
        <dt>{t("原资金余额")}</dt>
        <dd>{money(slice.balance)}</dd>
        <dt>{t("原权益")}</dt>
        <dd>{money(equity)}</dd>
        <dt>{t("原保证金占用")}</dt>
        <dd>{money(slice.funds?.margin ?? null)}</dd>
        <dt>{t("原可用资金")}</dt>
        <dd>{money(slice.funds?.available ?? null)}</dd>
        <dt>{t("原冻结资金")}</dt>
        <dd>{money(slice.funds?.frozen ?? null)}</dd>
        <dt>{t("所列净名义金额")}</dt>
        <dd>{money(slice.covered_net_notional)}</dd>
        <dt>{t("所列总名义金额")}</dt>
        <dd>{money(slice.covered_gross_notional)}</dd>
        <dt>{t("所列净名义/权益（倍）")}</dt>
        <dd>{observedRatio(slice.net_to_equity)}</dd>
        <dt>{t("所列总名义/权益（倍）")}</dt>
        <dd>{observedRatio(slice.gross_to_equity)}</dd>
        <dt>{t("原保证金/权益")}</dt>
        <dd>{observedRatio(slice.margin_to_equity, true)}</dd>
      </dl>
      {!slice.funds && (
        <p>{t("原结算资金观察未记录；不从余额、费率或其他时点重构保证金、可用或冻结。")}</p>
      )}
      <p>
        {t(
          "多空手数只按已记录总/净手数精确推导；集中度仅针对所列总名义金额。未归属风险不平均分摊，不跨品种抵扣保证金。",
        )}
      </p>
      <div style={{ overflowX: "auto" }}>
        <table aria-label={t("原合约敞口与单位证据")}>
          <thead>
            <tr>
              {[
                "原结算行",
                "原月份合约",
                "原价格（合约报价单位）",
                "原乘数",
                "原手数步长",
                "原净手数",
                "原总手数",
                "推导多手数",
                "推导空手数",
                "净名义金额",
                "总名义金额",
                "原保证金占用",
                "净名义/权益（倍）",
                "总名义/权益（倍）",
                "保证金/权益",
                "所列总额占比",
              ].map(label => (
                <th key={label}>{t(label)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {positions.map(row => (
              <tr key={row.source_contract_index}>
                <td>{row.source_contract_index}</td>
                <td>
                  {row.venue}/{row.symbol}
                  <br />
                  {row.product} · {row.delivery_month}
                </td>
                <td>{row.price}</td>
                <td>{row.multiplier}</td>
                <td>{row.quantity_increment}</td>
                <td>{row.net_quantity}</td>
                <td>{quantity(row.gross_quantity)}</td>
                <td>{quantity(row.long_quantity)}</td>
                <td>{quantity(row.short_quantity)}</td>
                <td>{money(row.net_notional)}</td>
                <td>{money(row.gross_notional)}</td>
                <td>{money(row.margin)}</td>
                <td>{observedRatio(row.net_to_equity)}</td>
                <td>{observedRatio(row.gross_to_equity)}</td>
                <td>{observedRatio(row.margin_to_equity, true)}</td>
                <td>{observedRatio(row.covered_gross_share, true)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <button disabled={page === 0} onClick={() => setPage(page - 1)}>
        {t("上一页敞口合约")}
      </button>
      <button
        disabled={(page + 1) * size >= slice.positions.length}
        onClick={() => setPage(page + 1)}
      >
        {t("下一页敞口合约")}
      </button>
      <p>
        {t(
          "净名义可相抵，双边总额、保证金、基差和退出风险不会因此消失。该切片不含完整策略归因、尾部估计或压力情景，不改变真实限额或下单。",
        )}
      </p>
    </section>
  );
}
