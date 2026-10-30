import { useState } from "react";
import type { BacktestMonth } from "../../src/bridge/client";
import { t } from "./shared";

export function MonthlyContributions({ months }: { months: BacktestMonth[] }) {
  const [page, setPage] = useState(0);
  const last = Math.max(0, Math.ceil(months.length / 24) - 1);
  const current = Math.min(page, last);
  return (
    <details>
      <summary>{t("按月收益贡献")}</summary>
      <p className="subtle">
        {t(
          "按记录中的交易日归月，只统计实际结算记录，不补齐缺失月份。净盈亏包含所有持仓的结算变化；扣费前盈亏仅加回手续费，仍含滑点影响。",
        )}
      </p>
      <p className="subtle">
        {t(
          "各月净盈亏除以初始资金，得到可相加的总收益率贡献，不是各月自身收益率或利润占比。覆盖结果的全部交易日，包含训练段；留出和滚动样本外绩效请看对应指标。",
        )}
      </p>
      <div className="research-table">
        <table aria-label={t("按月收益贡献")}>
          <thead>
            <tr>
              {[
                "月份",
                "实际结算区间",
                "交易日数",
                "期初权益",
                "期末权益",
                "扣费前盈亏",
                "手续费",
                "净盈亏",
                "对总收益率贡献（百分点）",
              ].map(key => (
                <th key={key}>{t(key)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {months.slice(current * 24, (current + 1) * 24).map(month => (
              <tr key={month.first_day}>
                <td>{month.first_day.slice(0, 7)}</td>
                <td>
                  {month.first_day} – {month.last_day}
                </td>
                <td>{month.trading_days}</td>
                <td>{month.opening_equity}</td>
                <td>{month.closing_equity}</td>
                <td>{month.gross}</td>
                <td>{month.fees}</td>
                <td>{month.net}</td>
                <td>{(100 * month.return_contribution).toFixed(2)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {last > 0 && (
        <div className="toolbar">
          <button disabled={current === 0} onClick={() => setPage(current - 1)}>
            {t("上一页")}
          </button>
          <span>
            {current + 1} / {last + 1}
          </span>
          <button disabled={current === last} onClick={() => setPage(current + 1)}>
            {t("下一页")}
          </button>
        </div>
      )}
    </details>
  );
}
