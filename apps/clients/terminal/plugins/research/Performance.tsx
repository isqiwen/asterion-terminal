import type { PerformanceFigures } from "../../src/bridge/client";
import { t } from "./shared";
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
    </section>
  );
}
