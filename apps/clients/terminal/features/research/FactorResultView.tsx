import { useMemo, useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { Period } from "../../src/generated/asterion/common_pb";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { type MessageValues, translate } from "../../src/i18n";
import { CopySummaryButton, ExperimentNoteBar, useAnnotations } from "./LabParts";
import { cumulativeIc, formatFactorSummaryMarkdown, yearlyIc } from "./research-lab";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// Daily bars are labelled at 15:00 Beijing time; minute bars carry YYYY-MM-DD HH:MM Beijing time.
const closeOf = (day: string) =>
  day.includes(" ")
    ? String(BigInt(Date.parse(`${day.replace(" ", "T")}:00+08:00`)) * 1_000_000n)
    : String(BigInt(Date.parse(`${day}T07:00:00Z`)) * 1_000_000n);
const formatTimeStamp = (ns: string, intraday: boolean) => {
  const iso = new Date(Number(BigInt(ns) / 1_000_000n) + 8 * 3600_000).toISOString();
  return intraday ? `${iso.slice(5, 10)} ${iso.slice(11, 16)}` : iso.slice(0, 10);
};
const periodTag = (period: Period | undefined): string => {
  switch (period) {
    case Period.MINUTES_1:
      return "1m";
    case Period.MINUTES_5:
      return "5m";
    case Period.MINUTES_10:
      return "10m";
    case Period.MINUTES_15:
      return "15m";
    case Period.MINUTES_30:
      return "30m";
    case Period.MINUTES_60:
      return "60m";
    default:
      return "";
  }
};

// Groups from the lowest values (cool cyan-blue) to the highest (warm amber).
const groupColor = (group: number, count: number) =>
  `hsl(${Math.round(204 - (164 * group) / Math.max(1, count - 1))}, 68%, 60%)`;

const shortFactor = (id: string) => id.split(".")[1] ?? id;

const correlationFill = (value: number | undefined, diagonal: boolean) => {
  if (value === undefined || !Number.isFinite(value)) return undefined;
  if (diagonal) return "color-mix(in oklab, var(--muted) 12%, var(--panel))";
  const strength = Math.round(8 + 52 * Math.min(1, Math.abs(value)));
  if (value > 0) return `color-mix(in oklab, var(--quote-up) ${strength}%, var(--panel))`;
  if (value < 0) return `color-mix(in oklab, var(--quote-down) ${strength}%, var(--panel))`;
  return "var(--panel)";
};

// A factor's evaluation, as Alphalens reports one: the first horizon's figures,
// IC by horizon, yearly IC stability, each day's IC and its running sum, and the groups.
export function FactorResultView({ result }: { result: FactorResult }) {
  const [tab, setTab] = useState<"overview" | "code" | "skipped">("overview");
  const [focusedDay, setFocusedDay] = useState<string | null>(null);
  const largest = Math.max(...result.horizons.map(item => Math.abs(item.mean ?? 0)), 1e-12);
  const widest = Math.max(...result.groups.map(item => Math.abs(item.meanReturn ?? 0)), 1e-12);
  const first = result.horizons[0];
  const { grouped, ic, yearly } = useMemo(
    () => ({
      grouped: result.days.filter(day => day.groups.length),
      ic: cumulativeIc(result.days),
      yearly: yearlyIc(result.days, first?.horizon ?? 1),
    }),
    [result.days, first?.horizon],
  );
  const dailyIcSeries = useMemo(
    () => ({
      kind: "histogram" as const,
      points: result.days.map(day => ({
        timestamp_ns: closeOf(day.tradingDay),
        price: day.ic === undefined ? "" : day.ic.toFixed(4),
      })),
    }),
    [result.days],
  );
  const cumulativeIcSeries = useMemo(
    () => ({
      kind: "line" as const,
      points: result.days.map((day, index) => ({
        timestamp_ns: closeOf(day.tradingDay),
        price: ic[index].toFixed(4),
      })),
    }),
    [result.days, ic],
  );
  const groupSeries = useMemo(
    () => ({
      kind: "line" as const,
      points: grouped.map(day => ({
        timestamp_ns: closeOf(day.tradingDay),
        price: String(day.longShort ?? 1),
      })),
    }),
    [grouped],
  );
  const hasNetLongShort = useMemo(
    () => grouped.some(day => day.netLongShort !== undefined),
    [grouped],
  );
  const groupOverlays = useMemo(
    () => [
      ...result.groups.map((group, index) => ({
        label: t("第 {group} 组", { group: group.group }),
        color: groupColor(index, result.groups.length),
        values: grouped.map(day => day.groups[index] ?? null),
      })),
      ...(hasNetLongShort
        ? [
            {
              label: t("扣费多空"),
              color: "#e2e8f0",
              values: grouped.map(day => day.netLongShort ?? null),
            },
          ]
        : []),
    ],
    [result.groups, grouped, hasNetLongShort],
  );
  const largestYearly = Math.max(...yearly.map(item => Math.abs(item.mean)), 1e-12);
  const positiveYears = yearly.filter(item => item.mean > 0).length;
  const longShort = grouped.at(-1)?.longShort;
  const netLongShort = grouped.at(-1)?.netLongShort;
  const composite = result.factorNames.length >= 2;
  const combinationLabel =
    result.evaluation?.combination === "ic_weighted" ? t("IC 加权合成") : t("等权合成");
  const evaluation = result.evaluation;
  const intraday = useMemo(
    () =>
      Boolean(periodTag(evaluation?.period)) || result.days.some(d => d.tradingDay.includes(" ")),
    [evaluation?.period, result.days],
  );
  const dayOf = (ns: string) => formatTimeStamp(ns, intraday);
  const pTag = periodTag(evaluation?.period);
  const hygieneTags = [
    ...(pTag ? [pTag] : []),
    ...(evaluation?.returnMode === "open" ? [t("次根开盘")] : []),
    ...(evaluation?.winsorize ? [t("MAD 去极值")] : []),
    ...(evaluation?.neutralize === "sector" ? [t("板块中性")] : []),
    ...(evaluation?.volAdjust ? [t("波动归一")] : []),
    ...(evaluation?.minVolume ? [t("量≥{value}", { value: evaluation.minVolume })] : []),
    ...(evaluation?.minOpenInterest
      ? [t("仓≥{value}", { value: evaluation.minOpenInterest })]
      : []),
    ...(evaluation?.costBps ? [t("成本 {value} bps", { value: evaluation.costBps })] : []),
    ...(evaluation?.orthogonalizeAgainst.length
      ? [
          t("正交剥离 {names}", {
            names: evaluation.orthogonalizeAgainst.map(item => shortFactor(item.factor)).join(", "),
          }),
        ]
      : []),
  ];
  const annotations = useAnnotations();
  const tabs = [
    ["overview", t("概览")],
    ["code", t("评价时的源码")],
    ...(result.skipped.length
      ? [["skipped", t("未参与（{count}）", { count: result.skipped.length })] as const]
      : []),
  ] as const;
  return (
    <div className="lab-result">
      <div className="lab-meta-head">
        <div className="lab-meta-main">
          <span className="lab-meta-title">
            {composite
              ? `${result.factorNames.join(" + ")} · ${combinationLabel}`
              : result.evaluation?.factor}
            {!composite &&
              Object.entries(result.evaluation?.params ?? {}).map(
                ([name, value]) => ` · ${name}=${String(value.value.value)}`,
              )}
          </span>
          <span className="subtle">
            {t("{count} 个品种", { count: result.symbols.length })} · {result.evaluation?.firstDay}{" "}
            – {result.evaluation?.lastDay}
          </span>
          {hygieneTags.length > 0 && (
            <span className="lab-meta-tags">
              {hygieneTags.map(tag => (
                <span key={tag} className="lab-meta-chip">
                  {tag}
                </span>
              ))}
            </span>
          )}
        </div>
        <span className="lab-meta-pinned subtle">
          {t("研究数据")} <code>{result.pinned?.datasetVersion.slice(0, 8)}</code>
          {result.pinned?.commit && (
            <>
              {" "}
              · {t("代码")} <code>{result.pinned.commit.slice(0, 8)}</code>
            </>
          )}
          {result.pinned?.changes && ` · ${t("含未提交改动")}`}
        </span>
      </div>
      <ExperimentNoteBar
        id={result.id}
        trailing={
          <CopySummaryButton
            build={() => formatFactorSummaryMarkdown(result, annotations[result.id]?.note)}
          />
        }
      />
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
              ...(first.tHac !== undefined
                ? [[t("HAC t 值"), metric(first.tHac, false), first.tHac] as const]
                : []),
              [t("IC 为正"), metric(first.positiveShare, true), undefined],
              ...(result.autocorr !== undefined
                ? [[t("信号自相关"), metric(result.autocorr, false), undefined] as const]
                : []),
              ...(result.halfLife !== undefined
                ? [
                    [
                      t("信号半衰期"),
                      t("{bars} 根", { bars: result.halfLife.toFixed(1) }),
                      undefined,
                    ] as const,
                  ]
                : []),
              [
                t("多空累计"),
                longShort === undefined ? "—" : metric(longShort - 1, true),
                longShort === undefined ? undefined : longShort - 1,
              ],
              ...(netLongShort !== undefined
                ? [[t("扣费多空累计"), metric(netLongShort - 1, true), netLongShort - 1] as const]
                : []),
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
          <button type="button" key={key} aria-pressed={tab === key} onClick={() => setTab(key)}>
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
          <h3>{t("IC 与衰减")}</h3>
          <table className="lab-table" aria-label={t("IC 与衰减")}>
            <thead>
              <tr>
                <th scope="col">{t("持有期")}</th>
                <th scope="col" className="numeric">
                  {t("IC 均值")}
                </th>
                <td aria-hidden="true" />
                <th scope="col" className="numeric">
                  {t("IC 标准差")}
                </th>
                <th scope="col" className="numeric">
                  ICIR
                </th>
                <th scope="col" className="numeric">
                  {t("t 值")}
                </th>
                <th scope="col" className="numeric">
                  {t("HAC t 值")}
                </th>
                <th scope="col" className="numeric">
                  {t("IC 为正")}
                </th>
                <th scope="col" className="numeric">
                  {t("天数")}
                </th>
              </tr>
            </thead>
            <tbody>
              {result.horizons.map(item => (
                <tr key={item.horizon}>
                  <td>{t("{count} 根", { count: item.horizon })}</td>
                  <td className={`numeric ${signed(item.mean)}`}>{metric(item.mean, false)}</td>
                  <td className="lab-bar-cell" aria-hidden="true">
                    <span
                      className={`lab-bar ${signed(item.mean)}`}
                      style={{ width: `${(100 * Math.abs(item.mean ?? 0)) / largest}%` }}
                    />
                  </td>
                  <td className="numeric">{metric(item.deviation, false)}</td>
                  <td className="numeric">{metric(item.ir, false)}</td>
                  <td className="numeric">{metric(item.t, false)}</td>
                  <td className="numeric">{metric(item.tHac, false)}</td>
                  <td className="numeric">{metric(item.positiveShare, true)}</td>
                  <td className="numeric">{item.days}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {result.developmentHorizons.length > 0 && result.holdoutHorizons.length > 0 && (
            <>
              <h3>{t("样本内外对比")}</h3>
              <table className="lab-table" aria-label={t("样本内外对比")}>
                <thead>
                  <tr>
                    <th scope="col">{t("持有期")}</th>
                    <th scope="col" className="numeric">
                      {t("样本内 IC")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("样本内 ICIR")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("留出期 IC")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("留出期 ICIR")}
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {result.developmentHorizons.map((dev, idx) => {
                    const out = result.holdoutHorizons[idx];
                    return (
                      <tr key={dev.horizon}>
                        <td>{t("{count} 根", { count: dev.horizon })}</td>
                        <td className={`numeric ${signed(dev.mean)}`}>{metric(dev.mean, false)}</td>
                        <td className="numeric">{metric(dev.ir, false)}</td>
                        <td className={`numeric ${signed(out?.mean)}`}>
                          {metric(out?.mean, false)}
                        </td>
                        <td className="numeric">{metric(out?.ir, false)}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </>
          )}
          {yearly.length > 0 && (
            <>
              <div className="lab-yearly-head">
                <h3>
                  {t("分年度 IC 稳定性（持有期 {count} 根）", { count: first?.horizon ?? 1 })}
                </h3>
                {yearly.length >= 2 && (
                  <span className="subtle">
                    {t("正 IC 年份 {positive} / {total}", {
                      positive: positiveYears,
                      total: yearly.length,
                    })}
                  </span>
                )}
              </div>
              <table className="lab-table" aria-label={t("分年度 IC 稳定性")}>
                <thead>
                  <tr>
                    <th scope="col">{t("年份")}</th>
                    <th scope="col" className="numeric">
                      {t("IC 均值")}
                    </th>
                    <td aria-hidden="true" />
                    <th scope="col" className="numeric">
                      {t("IC 标准差")}
                    </th>
                    <th scope="col" className="numeric">
                      ICIR
                    </th>
                    <th scope="col" className="numeric">
                      {t("t 值")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("IC 为正")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("天数")}
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {yearly.map(item => (
                    <tr key={item.year}>
                      <th scope="row">{item.year}</th>
                      <td className={`numeric ${signed(item.mean)}`}>{metric(item.mean, false)}</td>
                      <td className="lab-bar-cell" aria-hidden="true">
                        <span
                          className={`lab-bar ${signed(item.mean)}`}
                          style={{ width: `${(100 * Math.abs(item.mean)) / largestYearly}%` }}
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
            </>
          )}
          {result.factorNames.length >= 2 &&
            result.correlations.length === result.factorNames.length ** 2 && (
              <>
                <h3>{t("因子相关性热力图")}</h3>
                <div
                  className="lab-scroll"
                  tabIndex={0}
                  role="region"
                  aria-label={t("因子相关性热力图")}
                >
                  <table className="lab-heatmap lab-corr-heatmap" aria-label={t("因子相关性")}>
                    <thead>
                      <tr>
                        <th scope="col">{t("因子")}</th>
                        {result.factorNames.map(name => (
                          <th key={name} scope="col" title={name}>
                            {shortFactor(name)}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {result.factorNames.map((rowName, r) => (
                        <tr key={rowName}>
                          <th scope="row" title={rowName}>
                            {shortFactor(rowName)}
                          </th>
                          {result.factorNames.map((colName, c) => {
                            const correlation =
                              result.correlations[r * result.factorNames.length + c];
                            const value = correlation.value;
                            const background = correlationFill(value, r === c);
                            return (
                              <td
                                key={colName}
                                className={r === c ? "subtle" : undefined}
                                title={`${rowName} × ${colName}: ${metric(value, false)} · ${t("{days} 个有效交易日，{samples} 个品种日样本", { days: correlation.days, samples: correlation.samples })}`}
                                style={background ? { background } : undefined}
                              >
                                {metric(value, false)}
                              </td>
                            );
                          })}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </>
            )}
          {result.days.length > 0 && (
            <div className="lab-segments">
              <PriceChart
                series={dailyIcSeries}
                label={t("每日 IC（持有期 {count} 根）", { count: first?.horizon ?? 1 })}
                titled
                volumeLabel={t("成交量")}
                showVolume={false}
                focusedTime={focusedDay}
                onFocusChange={setFocusedDay}
                formatTime={dayOf}
                describe={index =>
                  `${result.days[index].tradingDay} · ${metric(result.days[index].ic, false)}`
                }
              />
              <PriceChart
                series={cumulativeIcSeries}
                label={t("累计 IC（持有期 {count} 根）", { count: first?.horizon ?? 1 })}
                titled
                volumeLabel={t("成交量")}
                showVolume={false}
                fillArea
                baselineValue={0}
                focusedTime={focusedDay}
                onFocusChange={setFocusedDay}
                formatTime={dayOf}
                describe={index => `${result.days[index].tradingDay} · ${ic[index].toFixed(3)}`}
              />
            </div>
          )}
          {grouped.length > 0 && (
            <PriceChart
              series={groupSeries}
              overlays={groupOverlays}
              label={t("分组累计收益（最高组减最低组为主线）")}
              titled
              volumeLabel={t("成交量")}
              showVolume={false}
              focusedTime={focusedDay}
              onFocusChange={setFocusedDay}
              formatTime={dayOf}
              describe={index => grouped[index].tradingDay}
            />
          )}
          <h3>{t("分组")}</h3>
          <table className="lab-table" aria-label={t("分组")}>
            <thead>
              <tr>
                <th scope="col">{t("分组（1 为因子值最低）")}</th>
                <th scope="col" className="numeric">
                  {t("平均每期收益")}
                </th>
                <td aria-hidden="true" />
                <th scope="col" className="numeric">
                  {t("换手率")}
                </th>
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
                  <td className="lab-bar-cell" aria-hidden="true">
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
