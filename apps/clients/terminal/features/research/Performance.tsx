import type { PerformanceFigures, PerformanceUnavailable } from "../../src/bridge/client";
import { t } from "./shared";
const reasons: Record<PerformanceUnavailable, string> = {
  source_time_not_recorded: "原观测时刻未记录",
  non_positive_observation_duration: "实际观测时长不为正",
  fewer_than_two_complete_returns: "不足两个完整结算收益周期",
  non_positive_equity_path: "区间中存在非正权益",
  annual_periods_not_configured: "年期数A未配置",
  risk_free_not_configured: "无风险收益未配置",
  constant_returns: "收益恒定，夏普分母为零",
  no_drawdown: "无回撤，卡玛分母为零",
  non_finite_statistic: "统计值超出有限数值范围",
  observation_shorter_than_one_year: "观测不足一年，不年化",
};
// Performance figures are statistics; an absent one was not computed.
export const percent = (value: number | null) =>
  value === null ? "—" : `${(value * 100).toFixed(2)}%`;
export const ratio = (value: number | null) => (value === null ? "—" : value.toFixed(2));
export function Figures({ label, value }: { label: string; value: PerformanceFigures }) {
  return (
    <section aria-label={label}>
      <div className="research-metrics">
        {[
          [t("总收益率"), percent(value.total_return)],
          [t("年化收益率"), percent(value.annual_return)],
          [t("年化波动率"), percent(value.annual_volatility)],
          [t("夏普比率"), ratio(value.sharpe)],
          [t("最大回撤"), percent(value.max_drawdown)],
          [t("卡玛比率"), ratio(value.calmar)],
          [t("盈利交易日占比"), percent(value.winning_days)],
          [t("交易日数"), value.trading_days],
        ].map(([name, figure]) => (
          <div key={name}>
            <span className="subtle">{name}</span>
            <strong>{figure}</strong>
          </div>
        ))}
      </div>
      {value.calculation ? (
        <details>
          <summary>{t("绩效方法与不可计算原因")}</summary>
          <dl>
            <dt>{t("原观测起点（UTC纳秒）")}</dt>
            <dd>{value.calculation.begin_ns ?? t("未记录")}</dd>
            <dt>{t("原观测终点（UTC纳秒）")}</dt>
            <dd>{value.calculation.end_ns ?? t("未记录")}</dd>
            <dt>{t("年期数A（日结算）")}</dt>
            <dd>{value.calculation.annual_periods ?? t("未配置")}</dd>
            <dt>{t("无风险收益假设")}</dt>
            <dd>
              {value.calculation.risk_free_annual === 0 ? t("明确零无风险") : t("无风险收益未配置")}
            </dd>
            <dt>{t("有效完整收益周期")}</dt>
            <dd>{value.calculation.return_periods}</dd>
          </dl>
          <p>
            {t(
              value.calculation.opening_return_included
                ? "起点为原结算，首个完整周期参与统计。"
                : "起点到首日结算为开端短周期，保留金额损益但不混入完整周期波动率和夏普。",
            )}
          </p>
          <p>
            {t(
              "年化收益按实际秒数除以365.2425天；波动率与夏普使用sqrt(A)缩放，未作自相关调整。这些是读取时统计，不替换原发布评分。",
            )}
          </p>
          {(
            [
              ["annual_return", "年化收益率"],
              ["annual_volatility", "年化波动率"],
              ["sharpe", "夏普比率"],
              ["calmar", "卡玛比率"],
            ] as const
          ).map(([key, name]) => {
            const reason = value.calculation?.unavailable[key];
            return reason ? (
              <p key={key}>
                {t(name)}: {t(reasons[reason])}
              </p>
            ) : null;
          })}
        </details>
      ) : (
        <p className="subtle">{t("本记录未提供明确绩效方法上下文。")}</p>
      )}
    </section>
  );
}
