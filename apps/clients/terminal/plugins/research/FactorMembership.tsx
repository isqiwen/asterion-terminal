import { useMemo, useState } from "react";
import type {
  FactorExperiment,
  FactorMembershipReason,
  FactorResult,
} from "../../src/bridge/client";
import { t } from "./shared";
const reasons: Record<FactorMembershipReason, string> = {
  MEMBERSHIP_ELIGIBLE: "有资格",
  MEMBERSHIP_UNOBSERVED: "未观测",
  MEMBERSHIP_INSUFFICIENT_HISTORY: "此前连续历史不足",
  MEMBERSHIP_UNKNOWN_VOLUME: "上一观测成交量未知",
  MEMBERSHIP_UNKNOWN_OPEN_INTEREST: "上一观测持仓量未知",
  MEMBERSHIP_BELOW_VOLUME: "上一观测成交量不足",
  MEMBERSHIP_BELOW_OPEN_INTEREST: "上一观测持仓量不足",
};
export function FactorMembership({
  experiment,
  result,
}: {
  experiment: Pick<FactorExperiment, "liquidity" | "series">;
  result: Pick<FactorResult, "memberships" | "cross_sections">;
}) {
  const days = useMemo(
    () => [...new Set(result.memberships.map(row => row.trading_day))].sort(),
    [result.memberships],
  );
  const [index, setIndex] = useState(0);
  const current = Math.min(index, Math.max(0, days.length - 1));
  const day = days[current];
  const rows = result.memberships.filter(row => row.trading_day === day);
  const section = result.cross_sections.find(row => row.observed === day);
  const rule = experiment.liquidity;
  if (!rule || !day) return null;
  return (
    <details>
      <summary>{t("每日资格与排除原因")}</summary>
      <p>
        {t("固定筛选规则")}: {t("此前连续观测数")} {rule.minimum_history} ·{" "}
        {t("上一观测最低成交量（手）")} {rule.minimum_volume} · {t("上一观测最低持仓量（手）")}{" "}
        {rule.minimum_open_interest}
      </p>
      <p className="subtle">
        {t(
          "资格来自原始日线时间轴，不随因子或未来收益改变。有资格仍需完整特征窗口和未来标签才能参与评价；预热、尾部及无期限点的日期可能没有评价截面。缺少本日收盘的成员显示未观测，不推断退市。固定快照不证明当时已发布。",
        )}
      </p>
      <p className="subtle">
        {t("品种使用上一主力观测月份的数量，不是所有月份合计；换月日不据此证明新月份当日流动性。")}
      </p>
      <div className="toolbar">
        <button disabled={current === 0} onClick={() => setIndex(current - 1)}>
          {t("上一交易日")}
        </button>
        <input
          aria-label={t("资格日期")}
          type="date"
          value={day}
          onChange={e => {
            const found = days.indexOf(e.target.value);
            if (found >= 0) setIndex(found);
          }}
        />
        <button disabled={current + 1 >= days.length} onClick={() => setIndex(current + 1)}>
          {t("下一交易日")}
        </button>
        <span>
          {t("实际评价成员数")}: {section ? section.series : "—"}
        </span>
      </div>
      <div className="research-table">
        <table aria-label={t("每日成员资格")}>
          <thead>
            <tr>
              <th>{t("品种或合约")}</th>
              <th>{t("资格或排除原因")}</th>
            </tr>
          </thead>
          <tbody>
            {experiment.series.map((series, s) => {
              const row = rows.find(row => row.series_index === s);
              const reason = row?.reason ?? "MEMBERSHIP_UNOBSERVED";
              return (
                <tr key={s}>
                  <td>{series.kind === "table" ? series.data.instrument : "—"}</td>
                  <td>{t(reasons[reason])}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </details>
  );
}
