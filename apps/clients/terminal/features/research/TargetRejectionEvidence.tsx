import type { TaskResult } from "../../src/bridge/client";
import { t } from "./shared";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;

export function TargetRejectionEvidence({ evidence }: { evidence: Evidence }) {
  const reasons = {
    order_quantity: "单笔数量上限",
    gross_quantity: "总持仓量上限",
    working_orders: "在途委托数上限",
    insufficient_funds: "保证金及手续费所需可用资金不足",
  } as const;
  if (!evidence.result.target_rejections?.length) return null;
  return (
    <table aria-label={t("模拟风控拒单原件")}>
      <thead>
        <tr>
          <th>{t("原行索引")}</th>
          <th>{t("新目标")}</th>
          <th>{t("拒单原因")}</th>
        </tr>
      </thead>
      <tbody>
        {evidence.result.target_rejections.map(row => (
          <tr key={row.intent_id}>
            <td>{row.source_index}</td>
            <td>{row.target}</td>
            <td>
              {t(reasons[row.reason])} <code>{row.intent_id}</code>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
