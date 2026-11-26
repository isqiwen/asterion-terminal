import { useEffect, useMemo, useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { lab } from "../../src/bridge/lab";
import type { RunDetail } from "../../src/generated/asterion/lab_pb";
import { useErrorNotification } from "../../src/host/notifications";
import { listSeparator, type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { NewModule, type ModuleDraft } from "./LabParts";
import { comparePortfolio } from "./research-analytics";
import { parameterText, runName } from "./research-lab";
import { combinedStrategyStem, combinedStrategyTemplate } from "./research-templates";
import { day, figures, metric, signed } from "./result-parts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// Colours of the compared runs after the first, which takes the chart's own.
const overlayColors = [
  "var(--series-1)",
  "var(--series-3)",
  "var(--series-4)",
  "var(--series-5)",
  "var(--series-2)",
];

// Runs side by side: their figures, the parameters that differ, their net
// value on the first run's days, correlation matrix, and portfolio synthesis.
export function RunComparison({
  ids,
  repository,
  onGenerated,
}: {
  ids: readonly string[];
  repository?: string;
  onGenerated?: (
    path: string,
    id: string,
    scope: { symbols: string; begin: string; end: string },
  ) => void;
}) {
  return (
    <Comparison
      key={ids.join("\n")}
      joined={ids.join("\n")}
      repository={repository}
      onGenerated={onGenerated}
    />
  );
}

// `joined` is the run ids, one a line: a parent's new array of the same ids reads nothing again.
function Comparison({
  joined,
  repository,
  onGenerated,
}: {
  joined: string;
  repository?: string;
  onGenerated?: (
    path: string,
    id: string,
    scope: { symbols: string; begin: string; end: string },
  ) => void;
}) {
  const ids = joined.split("\n");
  const [details, setDetails] = useState<RunDetail[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [simMode, setSimMode] = useState<"equal" | "inv_vol">("equal");
  const [generating, setGenerating] = useState<ModuleDraft | null>(null);
  useEffect(() => {
    let active = true;
    void Promise.all(joined.split("\n").map(id => lab({ case: "getRun", value: { id } })))
      .then(replies => active && setDetails(replies))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [joined]);
  const portfolio = useMemo(
    () => (details.length === ids.length ? comparePortfolio(details, simMode) : null),
    [details, ids.length, simMode],
  );
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (details.length !== ids.length) return <p className="subtle">{t("正在读取结果…")}</p>;
  const [first, ...others] = details;
  const name = (detail: RunDetail) => runName(detail.summary!);
  const base = (detail: RunDetail) => Number(JSON.parse(detail.record).costs.deposit);
  const seriesColor = (index: number) =>
    index === 0 ? "var(--price-line)" : overlayColors[(index - 1) % overlayColors.length];
  const rangeOf = (detail: RunDetail) =>
    detail.summary!.firstDay || detail.summary!.lastDay
      ? `${detail.summary!.firstDay} – ${detail.summary!.lastDay}`
      : "";
  const contextRows = (
    [
      [t("策略"), (detail: RunDetail) => detail.summary!.strategy],
      [t("品种"), (detail: RunDetail) => detail.summary!.symbols.join(listSeparator())],
      [t("区间"), rangeOf],
    ] as const
  ).filter(([, read]) => new Set(details.map(read)).size > 1);
  const diffRange = new Set(details.map(rangeOf)).size > 1;
  const params = [...new Set(details.flatMap(detail => Object.keys(detail.summary!.params)))];
  const differing = params.filter(
    param => new Set(details.map(detail => parameterText(detail.summary!.params[param]))).size > 1,
  );
  const distinctStrategies = new Set(details.map(d => d.summary!.strategy).filter(Boolean));
  const resolvedRepo =
    repository ||
    (details
      .map(
        d => (JSON.parse(d.record) as { strategy?: { repository?: string } }).strategy?.repository,
      )
      .find(Boolean) ??
      "");
  const canGenerateCombined = Boolean(
    portfolio && resolvedRepo && distinctStrategies.size >= 2 && onGenerated,
  );
  const portfolioByDay = portfolio
    ? new Map(portfolio.days.map((d, idx) => [d, portfolio.nav[idx]!]))
    : null;
  const corrCellStyle = (value: number | null) => {
    if (value === null) return undefined;
    const strength = Math.round(Math.min(1, Math.abs(value)) * 45);
    const tone = value >= 0 ? "var(--quote-up)" : "var(--quote-down)";
    return { background: `color-mix(in oklab, ${tone} ${strength}%, var(--panel))` };
  };
  return (
    <div className="lab-result">
      <div className="lab-scroll">
        <table className="lab-table lab-compare">
          <thead>
            <tr>
              <th scope="col">
                <span className="sr-only">{t("指标")}</span>
              </th>
              {details.map((detail, index) => (
                <th scope="col" key={detail.summary!.id} className="numeric">
                  <span
                    className="lab-swatch"
                    aria-hidden="true"
                    style={{ background: seriesColor(index) }}
                  />
                  {name(detail)}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {[...figures, ["fills", "成交笔数", false] as const].map(
              ([metricName, label, percent]) => (
                <tr key={metricName}>
                  <td>{t(label)}</td>
                  {details.map(detail => (
                    <td
                      key={detail.summary!.id}
                      className={`numeric ${metricName.endsWith("return") ? signed(detail.summary!.metrics[metricName]) : ""}`}
                    >
                      {metricName === "fills"
                        ? (detail.summary!.metrics.fills ?? "—")
                        : metric(detail.summary!.metrics[metricName], percent)}
                    </td>
                  ))}
                </tr>
              ),
            )}
            {contextRows.map(([label, read]) => (
              <tr key={label} className="lab-differs">
                <td>{label}</td>
                {details.map(detail => (
                  <td key={detail.summary!.id} className="numeric">
                    {read(detail) || "—"}
                  </td>
                ))}
              </tr>
            ))}
            {differing.map(param => (
              <tr key={param} className="lab-differs">
                <td>{param}</td>
                {details.map(detail => (
                  <td key={detail.summary!.id} className="numeric">
                    {parameterText(detail.summary!.params[param])}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {diffRange && (
        <p className="subtle">
          {t("净值曲线按首次运行（{name}）的交易日对齐", { name: name(first) })}
        </p>
      )}
      <PriceChart
        series={{
          kind: "line",
          points: first.days.map(item => ({
            timestamp_ns: String(item.settledMs * 1_000_000n),
            price: String(Number(item.equity?.text) / base(first)),
          })),
        }}
        overlays={[
          ...others.map((detail, index) => {
            const equity = new Map(
              detail.days.map(item => [item.tradingDay, Number(item.equity?.text)]),
            );
            return {
              label: name(detail),
              color: overlayColors[index % overlayColors.length]!,
              values: first.days.map(item => {
                const value = equity.get(item.tradingDay);
                return value === undefined ? null : value / base(detail);
              }),
            };
          }),
          ...(portfolioByDay
            ? [
                {
                  label: t("组合试算"),
                  color: "var(--accent)",
                  values: first.days.map(item => portfolioByDay.get(item.tradingDay) ?? null),
                },
              ]
            : []),
        ]}
        label={t("净值：{names}", { names: details.map(name).join(listSeparator()) })}
        titled
        volumeLabel={t("成交量")}
        showVolume={false}
        formatTime={value => day(BigInt(value))}
        describe={index =>
          first.days[index]
            ? `${first.days[index].tradingDay} · ${name(first)}: ${(Number(first.days[index].equity?.text) / base(first)).toFixed(4)}`
            : null
        }
      />
      {portfolio && (
        <>
          <div className="lab-toolbar">
            <h3>{t("多策略组合试算")}</h3>
            <div className="lab-segmented" role="group" aria-label={t("合成方式")}>
              {(
                [
                  ["equal", "等权合成"],
                  ["inv_vol", "波动率平价"],
                ] as const
              ).map(([mode, label]) => (
                <button
                  key={mode}
                  type="button"
                  aria-pressed={simMode === mode}
                  onClick={() => setSimMode(mode)}
                >
                  {t(label)}
                </button>
              ))}
            </div>
            <span className="lab-spacer" />
            {canGenerateCombined && (
              <button
                type="button"
                className="primary"
                onClick={() =>
                  setGenerating({
                    title: "生成组合策略",
                    name: combinedStrategyStem(details),
                    build: moduleName =>
                      combinedStrategyTemplate(
                        details,
                        portfolio.weights,
                        simMode === "inv_vol",
                        moduleName,
                      ),
                  })
                }
              >
                {t("生成组合策略")}
              </button>
            )}
          </div>
          <dl className="lab-figures">
            <div>
              <dt>{t("组合总收益")}</dt>
              <dd className={signed(portfolio.totalReturn)}>
                {metric(portfolio.totalReturn, true)}
              </dd>
            </div>
            <div>
              <dt>{t("组合夏普")}</dt>
              <dd>{metric(portfolio.sharpe ?? undefined, false)}</dd>
            </div>
            <div>
              <dt>{t("组合最大回撤")}</dt>
              <dd>{metric(portfolio.maxDrawdown, true)}</dd>
            </div>
            <div>
              <dt>{t("平均相关性")}</dt>
              <dd>{metric(portfolio.avgCorrelation ?? undefined, false)}</dd>
            </div>
            <div>
              <dt>{t("波动降幅")}</dt>
              <dd className={signed(portfolio.volReduction)}>
                {metric(portfolio.volReduction ?? undefined, true)}
              </dd>
            </div>
          </dl>
          <p className="subtle">
            {details
              .map(
                (detail, idx) => `${name(detail)} ${(portfolio.weights[idx]! * 100).toFixed(1)}%`,
              )
              .join(" · ")}
          </p>
          <h3>{t("策略相关性矩阵")}</h3>
          <div className="lab-scroll">
            <table className="lab-heatmap lab-corr-heatmap" aria-label={t("策略相关性矩阵")}>
              <thead>
                <tr>
                  <th scope="col" />
                  {details.map(detail => (
                    <th scope="col" key={detail.summary!.id}>
                      {name(detail)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {details.map((rowDetail, r) => (
                  <tr key={rowDetail.summary!.id}>
                    <th scope="row">{name(rowDetail)}</th>
                    {portfolio.correlations[r]!.map((val, c) => (
                      <td key={c} style={corrCellStyle(val)}>
                        {val === null ? "—" : val.toFixed(3)}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {generating && resolvedRepo && (
            <NewModule
              hideTrigger
              folder="strategies"
              repository={resolvedRepo}
              generated={generating}
              cancelGenerated={() => setGenerating(null)}
              created={(path, id) => {
                const symbols = [...new Set(details.flatMap(d => d.summary!.symbols))].join(", ");
                const begin = portfolio.days[0] ?? "";
                const end = portfolio.days.at(-1) ?? "";
                onGenerated?.(path, id ?? "", { symbols, begin, end });
              }}
            />
          )}
        </>
      )}
    </div>
  );
}
