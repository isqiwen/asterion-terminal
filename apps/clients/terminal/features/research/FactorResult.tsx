import { PriceChart } from "@asterion/client-ui/PriceChart";
import { translate, type LabFactorResult, type MessageValues } from "../contract";
import { cumulativeIc } from "./research-lab";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// Bars are labelled at 15:00 Beijing time, the close of their trading day.
const closeOf = (day: string) => String(BigInt(Date.parse(`${day}T07:00:00Z`)) * 1_000_000n);
const dayOf = (ns: string) =>
  new Date(Number(BigInt(ns) / 1_000_000n) + 8 * 3600_000).toISOString().slice(0, 10);

// Groups from the lowest values (blue) to the highest (orange).
const groupColor = (group: number, count: number) =>
  `hsl(${210 - (190 * group) / Math.max(1, count - 1)} 65% 58%)`;

// A factor's evaluation: IC by horizon, its running sum, and the groups.
export function FactorResult({ result }: { result: LabFactorResult }) {
  const largest = Math.max(...result.horizons.map(item => Math.abs(item.mean ?? 0)), 1e-12);
  const grouped = result.days.filter(day => day.groups.length);
  const ic = cumulativeIc(result.days);
  return (
    <div className="lab-result">
      <p className="subtle">
        {t("{count} 个品种", { count: result.symbols.length })} · {t("数据")}{" "}
        <code>{result.datasetVersion.slice(0, 8)}</code> · {t("策略仓库")}{" "}
        <code>{result.commit.slice(0, 8)}</code>
        {result.changes && ` · ${t("含未提交改动")}`}
      </p>
      {result.skipped.length > 0 && (
        <details className="lab-section">
          <summary>{t("未参与（{count}）", { count: result.skipped.length })}</summary>
          <ul className="lab-skipped">
            {result.skipped.map(item => (
              <li key={item.symbol}>
                <strong>{item.symbol}</strong> {item.reason}
              </li>
            ))}
          </ul>
        </details>
      )}
      <h5>{t("IC 与衰减")}</h5>
      <table className="lab-table" aria-label={t("IC 与衰减")}>
        <thead>
          <tr>
            <th>{t("持有期")}</th>
            <th className="numeric">{t("IC 均值")}</th>
            <th />
            <th className="numeric">{t("IC 标准差")}</th>
            <th className="numeric">ICIR</th>
            <th className="numeric">{t("t 值")}</th>
            <th className="numeric">{t("IC 为正")}</th>
            <th className="numeric">{t("天数")}</th>
          </tr>
        </thead>
        <tbody>
          {result.horizons.map(item => (
            <tr key={item.horizon}>
              <td>{t("{count} 根", { count: item.horizon })}</td>
              <td className="numeric">{metric(item.mean, false)}</td>
              <td className="lab-bar-cell">
                <span
                  className={`lab-bar ${signed(item.mean)}`}
                  style={{ width: `${(100 * Math.abs(item.mean ?? 0)) / largest}%` }}
                />
              </td>
              <td className="numeric">{metric(item.deviation, false)}</td>
              <td className="numeric">{metric(item.ir, false)}</td>
              <td className="numeric">{metric(item.t, false)}</td>
              <td className="numeric">{metric(item.positive, true)}</td>
              <td className="numeric">{item.days}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {result.days.length > 0 && (
        <PriceChart
          series={{
            kind: "line",
            points: result.days.map((day, index) => ({
              timestamp_ns: closeOf(day.tradingDay),
              price: String(ic[index]),
            })),
          }}
          label={t("累计 IC（持有期 {count} 根）", { count: result.horizons[0]?.horizon ?? 1 })}
          volumeLabel={t("成交量")}
          showVolume={false}
          fillArea
          formatTime={dayOf}
          describe={index => `${result.days[index].tradingDay} · ${ic[index].toFixed(3)}`}
        />
      )}
      {grouped.length > 0 && (
        <PriceChart
          series={{
            kind: "line",
            points: grouped.map(day => ({
              timestamp_ns: closeOf(day.tradingDay),
              price: String(day.longShort ?? 1),
            })),
          }}
          overlays={result.groups.map((group, index) => ({
            label: t("第 {group} 组", { group: group.group }),
            color: groupColor(index, result.groups.length),
            values: grouped.map(day => day.groups[index] ?? null),
          }))}
          label={t("分组累计收益（最高组减最低组为主线）")}
          volumeLabel={t("成交量")}
          showVolume={false}
          formatTime={dayOf}
          describe={index => grouped[index].tradingDay}
        />
      )}
      <h5>{t("分组")}</h5>
      <table className="lab-table" aria-label={t("分组")}>
        <thead>
          <tr>
            <th>{t("分组（1 为因子值最低）")}</th>
            <th className="numeric">{t("平均每期收益")}</th>
            <th className="numeric">{t("换手率")}</th>
          </tr>
        </thead>
        <tbody>
          {result.groups.map((group, index) => (
            <tr key={group.group}>
              <td>
                <span
                  className="lab-swatch"
                  style={{ background: groupColor(index, result.groups.length) }}
                />
                {group.group}
              </td>
              <td className={`numeric ${signed(group.meanReturn)}`}>
                {group.meanReturn === undefined ? "—" : `${(group.meanReturn * 100).toFixed(3)}%`}
              </td>
              <td className="numeric">{metric(group.turnover, true)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
