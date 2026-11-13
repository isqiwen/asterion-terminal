import { useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { type MessageValues, translate } from "../../src/i18n";
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

// A factor's evaluation, as Alphalens reports one: the first horizon's figures,
// IC by horizon, each day's IC and its running sum, and the groups.
export function FactorResultView({ result }: { result: FactorResult }) {
  const [tab, setTab] = useState<"overview" | "code" | "skipped">("overview");
  const largest = Math.max(...result.horizons.map(item => Math.abs(item.mean ?? 0)), 1e-12);
  const widest = Math.max(...result.groups.map(item => Math.abs(item.meanReturn ?? 0)), 1e-12);
  const grouped = result.days.filter(day => day.groups.length);
  const ic = cumulativeIc(result.days);
  const first = result.horizons[0];
  const longShort = grouped.at(-1)?.longShort;
  const tabs = [
    ["overview", t("概览")],
    ["code", t("评价时的源码")],
    ...(result.skipped.length
      ? [["skipped", t("未参与（{count}）", { count: result.skipped.length })] as const]
      : []),
  ] as const;
  return (
    <div className="lab-result">
      <p className="subtle">
        {result.evaluation?.factor}
        {Object.entries(result.evaluation?.params ?? {}).map(
          ([name, value]) => ` · ${name}=${String(value.value.value)}`,
        )}{" "}
        · {t("{count} 个品种", { count: result.symbols.length })} · {result.evaluation?.firstDay} –{" "}
        {result.evaluation?.lastDay} · {t("研究数据")}{" "}
        <code>{result.pinned?.datasetVersion.slice(0, 8)}</code>
        {result.pinned?.commit && (
          <>
            {" "}
            · {t("代码")} <code>{result.pinned.commit.slice(0, 8)}</code>
          </>
        )}
        {result.pinned?.changes && ` · ${t("含未提交改动")}`}
      </p>
      {first && (
        <dl className="lab-figures">
          {(
            [
              [
                t("IC 均值（{count} 根）", { count: first.horizon }),
                metric(first.mean, false),
                first.mean,
              ],
              ["ICIR", metric(first.ir, false), first.ir],
              [t("t 值"), metric(first.t, false), first.t],
              [t("IC 为正"), metric(first.positiveShare, true), undefined],
              [
                t("多空累计"),
                longShort === undefined ? "—" : metric(longShort - 1, true),
                longShort === undefined ? undefined : longShort - 1,
              ],
            ] as const
          ).map(([label, text, value]) => (
            <div key={label}>
              <dt>{label}</dt>
              <dd className={signed(value)}>{text}</dd>
            </div>
          ))}
        </dl>
      )}
      <nav className="lab-report-tabs tabs" aria-label={t("报告")}>
        {tabs.map(([key, label]) => (
          <button key={key} aria-pressed={tab === key} onClick={() => setTab(key)}>
            {label}
          </button>
        ))}
      </nav>
      {tab === "code" && <pre className="lab-code">{result.code}</pre>}
      {tab === "skipped" && (
        <ul className="lab-skipped">
          {result.skipped.map(item => (
            <li key={item.symbol}>
              <strong>{item.symbol}</strong> {item.reason}
            </li>
          ))}
        </ul>
      )}
      {tab === "overview" && (
        <>
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
                  <td className="numeric">{metric(item.positiveShare, true)}</td>
                  <td className="numeric">{item.days}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {result.days.length > 0 && (
            <div className="lab-segments">
              <PriceChart
                series={{
                  kind: "line",
                  points: result.days.map(day => ({
                    timestamp_ns: closeOf(day.tradingDay),
                    price: day.ic === undefined ? "" : day.ic.toFixed(4),
                  })),
                }}
                label={t("每日 IC（持有期 {count} 根）", { count: first?.horizon ?? 1 })}
                volumeLabel={t("成交量")}
                showVolume={false}
                formatTime={dayOf}
                describe={index =>
                  `${result.days[index].tradingDay} · ${metric(result.days[index].ic, false)}`
                }
              />
              <PriceChart
                series={{
                  kind: "line",
                  points: result.days.map((day, index) => ({
                    timestamp_ns: closeOf(day.tradingDay),
                    price: ic[index].toFixed(4),
                  })),
                }}
                label={t("累计 IC（持有期 {count} 根）", { count: first?.horizon ?? 1 })}
                volumeLabel={t("成交量")}
                showVolume={false}
                fillArea
                formatTime={dayOf}
                describe={index => `${result.days[index].tradingDay} · ${ic[index].toFixed(3)}`}
              />
            </div>
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
                <th />
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
                    {group.meanReturn === undefined
                      ? "—"
                      : `${(group.meanReturn * 100).toFixed(3)}%`}
                  </td>
                  <td className="lab-bar-cell">
                    <span
                      className={`lab-bar ${signed(group.meanReturn)}`}
                      style={{ width: `${(100 * Math.abs(group.meanReturn ?? 0)) / widest}%` }}
                    />
                  </td>
                  <td className="numeric">{metric(group.turnover, true)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </div>
  );
}
