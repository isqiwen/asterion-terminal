import { useState } from "react";
import type { BacktestTrades } from "../../src/bridge/client";
import { t } from "./shared";

export function TradeStatistics({ trades }: { trades: BacktestTrades }) {
  const [page, setPage] = useState(0);
  const last = Math.max(0, Math.ceil(trades.closed.length / 50) - 1);
  const current = Math.min(page, last);
  const holding = trades.holding_summary;
  const spanSummary = (
    value: { minimum: number | null; mean: number | null; maximum: number | null } | undefined,
  ) =>
    value?.mean == null
      ? t("未知（无成交日期证据）")
      : `${value.mean.toFixed(2)} (${value.minimum}–${value.maximum})`;
  const ratio = (value: number | null) => (value === null ? "—" : value.toFixed(2));
  return (
    <section aria-label={t("按笔统计")}>
      <h4>{t("按笔统计")}</h4>
      <div className="research-metrics">
        {[
          [t("平仓笔数"), trades.count],
          [
            t("净盈利笔数占比"),
            trades.count ? `${((100 * trades.winning) / trades.count).toFixed(2)}%` : "—",
          ],
          [t("平均净盈利"), ratio(trades.average_win)],
          [t("平均净亏损"), ratio(trades.average_loss)],
          [t("净盈亏比"), ratio(trades.payoff)],
          [t("已平仓毛盈亏"), trades.gross],
          [t("已平仓手续费"), trades.fees],
          [t("已平仓净盈亏"), trades.net],
          [t("未闭合持仓手续费"), trades.open_fees],
        ].map(([name, figure]) => (
          <div key={name}>
            <span className="subtle">{name}</span>
            <strong>{figure}</strong>
          </div>
        ))}
      </div>
      <p className="subtle">
        {t(
          "一笔是一个合约从空仓到再次空仓的一轮持仓。净盈亏扣除账户实际收取的全部开平仓手续费，胜负按净盈亏判断；主力换月的新旧月份分别统计。未闭合持仓的费用单列，已平仓净盈亏不等于含未平仓盈亏的账户总收益。",
        )}
      </p>
      <details>
        <summary>{t("持有期（交易日期口径）")}</summary>
        <p className="subtle">
          {t(
            "按首次开仓成交至最终平仓成交计算一轮，增减仓不重置起点；换月合约各自统计。日期日历日差是交易日期标签之差，结算日跨度只计该合约已记录的结算日（开仓日之后至结束日，含结束日）。同日均为0，不代表持有0分钟。",
          )}
        </p>
        <p className="subtle">
          {t(
            "仅未来单合约日线结果记录成交交易日，不提供真实盘中时间、小时或分钟持有期。未记录证据的结果保持未知，不从权益观测或订单ID倒推，也不会自动重算原实验。",
          )}
        </p>
        <div className="research-metrics">
          {[
            [
              t("已平仓跨度已知 / 未知"),
              `${holding?.known ?? 0} / ${holding?.unknown ?? trades.closed.length}`,
            ],
            [t("平均日期日历日差（最小–最大）"), spanSummary(holding?.calendar_days)],
            [t("平均结算日跨度（最小–最大）"), spanSummary(holding?.settlement_days)],
          ].map(([name, value]) => (
            <div key={name}>
              <span className="subtle">{name}</span>
              <strong>{value}</strong>
            </div>
          ))}
        </div>
        <p className="subtle">
          {t(
            "汇总仅包含持有日期已知的已平仓轮次；未知轮次和未平仓不计入平均、最小或最大跨度。按轮次等权平均，不按手数或盈亏加权。",
          )}
        </p>
        {!!trades.closed.length && (
          <table aria-label={t("已平仓持有期")}>
            <thead>
              <tr>
                {["序号", "合约", "开仓交易日", "平仓交易日", "日期日历日差", "结算日跨度"].map(
                  key => (
                    <th key={key}>{t(key)}</th>
                  ),
                )}
              </tr>
            </thead>
            <tbody>
              {trades.closed.slice(current * 50, (current + 1) * 50).map((trade, i) => (
                <tr key={current * 50 + i}>
                  <td>{current * 50 + i + 1}</td>
                  <td>
                    {trade.venue} · {trade.symbol}
                  </td>
                  <td>{trade.holding?.opened_day ?? t("未知（无成交日期证据）")}</td>
                  <td>{trade.holding?.ended_day ?? t("未知（无成交日期证据）")}</td>
                  <td>{trade.holding?.calendar_days ?? "—"}</td>
                  <td>{trade.holding?.settlement_days ?? "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        {!!trades.open?.length && (
          <>
            <h5>{t("未平仓持有跨度")}</h5>
            <p className="subtle">
              {t("未平仓仅截至结果末次结算观测，不当作已平仓持有期；不会假定平仓时间。")}
            </p>
            <table aria-label={t("未平仓持有跨度")}>
              <thead>
                <tr>
                  {[
                    "合约",
                    "方向",
                    "数量",
                    "开仓交易日",
                    "截至结算日",
                    "日期日历日差",
                    "结算日跨度",
                  ].map(key => (
                    <th key={key}>{t(key)}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {trades.open.map(trade => (
                  <tr key={`${trade.venue}.${trade.symbol}`}>
                    <td>
                      {trade.venue} · {trade.symbol}
                    </td>
                    <td>{t(trade.direction === "long" ? "多头" : "空头")}</td>
                    <td>{trade.quantity}</td>
                    <td>{trade.holding?.opened_day ?? t("未知（无成交日期证据）")}</td>
                    <td>{trade.as_of_day}</td>
                    <td>{trade.holding?.calendar_days ?? "—"}</td>
                    <td>{trade.holding?.settlement_days ?? "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        )}
      </details>
      {!!trades.by_contract.length && (
        <details>
          <summary>{t("按合约已平仓贡献")}</summary>
          <p className="subtle">
            {t("仅汇总已闭合交易轮次；不含未平仓盈亏及其手续费，各月份合约分别统计。")}
          </p>
          <div className="research-table">
            <table aria-label={t("按合约已平仓贡献")}>
              <thead>
                <tr>
                  {["合约", "平仓笔数", "毛盈亏", "手续费", "净盈亏"].map(key => (
                    <th key={key}>{t(key)}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {trades.by_contract.map(contribution => (
                  <tr key={`${contribution.venue}.${contribution.symbol}`}>
                    <td>
                      {contribution.venue} · {contribution.symbol}
                    </td>
                    <td>{contribution.count}</td>
                    <td>{contribution.gross}</td>
                    <td>{contribution.fees}</td>
                    <td>{contribution.net}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </details>
      )}
      {!!trades.closed.length && (
        <details>
          <summary>{t("逐笔费用与净盈亏")}</summary>
          <div className="research-table">
            <table aria-label={t("逐笔费用与净盈亏")}>
              <thead>
                <tr>
                  {["序号", "合约", "毛盈亏", "手续费", "净盈亏"].map(key => (
                    <th key={key}>{t(key)}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {trades.closed.slice(current * 50, (current + 1) * 50).map((trade, index) => (
                  <tr key={current * 50 + index}>
                    <td>{current * 50 + index + 1}</td>
                    <td>
                      {trade.venue} · {trade.symbol}
                    </td>
                    <td>{trade.gross}</td>
                    <td>{trade.fees}</td>
                    <td>{trade.net}</td>
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
      )}
    </section>
  );
}
