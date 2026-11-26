import { useState } from "react";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import { listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import { breakEvenSlippage, deflatedSharpeRatio, heatmap, medianValue } from "./research-analytics";
import { batchKey, batchStrategy, parameterText, runState, slippageKey } from "./research-lab";
import {
  metric,
  metricLabel,
  percentMetric,
  signed,
  states,
  VirtualScrollTable,
} from "./result-parts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// A sweep's runs ranked by a metric, and with two swept dimensions a grid of it.
export function SweepResult({
  runs,
  selected,
  pbo,
  pboRankCorr,
  open,
  rolling,
}: {
  runs: readonly RunSummary[];
  selected: string;
  pbo?: number;
  pboRankCorr?: number;
  open: (id: string) => void;
  rolling?: () => void;
}) {
  const names = Object.keys(runs[0]?.params ?? {});
  const hasSymbols = new Set(runs.map(run => run.symbols.join(","))).size > 1;
  const symbolAxis = "__symbol__";
  const formatSymbols = (symbols: readonly string[]) =>
    symbols
      .map(symbol => {
        const label = warehouseName(symbol);
        return label ? `${label} ${symbol}` : symbol;
      })
      .join(listSeparator());
  const [measure, setMeasure] = useState<string>(
    runs.some(run => "development_sharpe" in run.metrics) ? "development_sharpe" : "sharpe",
  );
  const [sortBy, setSortBy] = useState<"metric" | "plateau">("metric");
  const [rowAxisChoice, setRowAxisChoice] = useState("");
  const [colAxisChoice, setColAxisChoice] = useState("");
  const baseRows = runs.map(run => ({
    id: run.id,
    state: runState(run.state),
    symbols: formatSymbols(run.symbols),
    span: run.firstDay && run.lastDay ? `${run.firstDay} – ${run.lastDay}` : "",
    metrics: run.metrics,
    params: Object.fromEntries([
      ...(hasSymbols ? [[symbolAxis, formatSymbols(run.symbols)]] : []),
      ...names.map(name => [
        name,
        name === batchKey ? batchStrategy(run).split(".")[1] : parameterText(run.params[name]),
      ]),
    ]),
    metric: run.metrics[measure] ?? null,
    plateau: null as number | null,
  }));
  const lowerBetter = /drawdown|volatility|ulcer/.test(measure);
  const sweptParams = names.filter(name => new Set(baseRows.map(row => row.params[name])).size > 1);
  const sweptAxes = [...(hasSymbols ? [symbolAxis] : []), ...sweptParams];
  const axisLabel = (axis: string) =>
    axis === symbolAxis
      ? t("品种")
      : axis === batchKey
        ? t("策略")
        : axis === slippageKey
          ? t("滑点（跳）")
          : axis;
  const isSlippage = names.length === 1 && names[0] === slippageKey;
  const pureMultiSymbol = (hasSymbols && names.length === 0) || names[0] === batchKey || isSlippage;
  const hasPlateau = !pureMultiSymbol && !isSlippage && sweptAxes.length >= 1;

  // Compute N-D immediate grid neighborhood median (axial Manhattan-1, Chebyshev-1 fallback).
  const axisOrder = new Map(
    sweptAxes.map(axis => {
      const sortedVals = [...new Set(baseRows.map(row => row.params[axis]))].sort(
        (a, b) => Number(a) - Number(b) || a.localeCompare(b),
      );
      return [axis, new Map(sortedVals.map((val, idx) => [val, idx]))];
    }),
  );
  const coords = hasPlateau
    ? baseRows.map(row => sweptAxes.map(axis => axisOrder.get(axis)?.get(row.params[axis]) ?? 0))
    : [];
  const neighborMetricsOf = (
    idx: number,
    valueAt: (rowIndex: number) => number | null | undefined,
  ) => {
    const ci = coords[idx]!;
    const axial: number[] = [];
    const diagonal: number[] = [];
    for (let j = 0; j < baseRows.length; j++) {
      if (idx === j) continue;
      const val = valueAt(j);
      if (val === null || val === undefined || !Number.isFinite(val)) continue;
      const cj = coords[j]!;
      let manhattan = 0;
      let maxDiff = 0;
      for (let d = 0; d < ci.length; d++) {
        const diff = Math.abs(ci[d]! - cj[d]!);
        manhattan += diff;
        if (diff > maxDiff) maxDiff = diff;
      }
      if (manhattan === 1) axial.push(val);
      else if (maxDiff <= 1) diagonal.push(val);
    }
    return axial.length > 0 ? axial : diagonal;
  };
  const rows = baseRows.map((row, i) => {
    if (!hasPlateau) return row;
    return { ...row, plateau: medianValue(neighborMetricsOf(i, j => baseRows[j]!.metric)) };
  });

  const plateauRankScore = (row: (typeof rows)[number]) => {
    if (row.metric === null) return null;
    if (row.plateau === null) return row.metric;
    return lowerBetter ? Math.max(row.metric, row.plateau) : Math.min(row.metric, row.plateau);
  };
  const ranked = [...rows].sort((a, b) => {
    const va = sortBy === "plateau" && hasPlateau ? plateauRankScore(a) : a.metric;
    const vb = sortBy === "plateau" && hasPlateau ? plateauRankScore(b) : b.metric;
    if (va === null) return 1;
    if (vb === null) return -1;
    if (va !== vb) return lowerBetter ? va - vb : vb - va;
    if (a.metric === null) return 1;
    if (b.metric === null) return -1;
    return lowerBetter ? a.metric - b.metric : b.metric - a.metric;
  });

  // Selectable 2D projection heatmap for 2D or higher-dimensional sweeps.
  const rowAxis =
    sweptAxes.length >= 2
      ? sweptAxes.includes(rowAxisChoice)
        ? rowAxisChoice
        : sweptAxes[0]!
      : "";
  const colAxis =
    sweptAxes.length >= 2
      ? sweptAxes.includes(colAxisChoice) && colAxisChoice !== rowAxis
        ? colAxisChoice
        : (sweptAxes.find(axis => axis !== rowAxis) ?? "")
      : "";
  const heatmapProjection = (() => {
    if (!rowAxis || !colAxis) return null;
    const bestByCell = new Map<string, (typeof rows)[number]>();
    for (const row of rows) {
      const key = `${row.params[rowAxis]}\0${row.params[colAxis]}`;
      const existing = bestByCell.get(key);
      if (!existing) {
        bestByCell.set(key, row);
        continue;
      }
      if (
        row.metric !== null &&
        (existing.metric === null ||
          (lowerBetter ? row.metric < existing.metric : row.metric > existing.metric))
      ) {
        bestByCell.set(key, row);
      }
    }
    const projected = [...bestByCell.values()];
    return { grid: heatmap(projected, rowAxis, colAxis), cellRuns: bestByCell };
  })();
  const grid = heatmapProjection?.grid ?? null;
  const cellRuns = heatmapProjection?.cellRuns ?? null;
  const values = rows.flatMap(row => (row.metric === null ? [] : [row.metric]));
  const low = Math.min(...values),
    high = Math.max(...values);
  const percent = percentMetric(measure);
  const extraMeasure = !["total_return", "sharpe", "max_drawdown"].includes(measure);

  const slippageSummary = isSlippage
    ? breakEvenSlippage(
        runs
          .filter(run => runState(run.state) === "succeeded")
          .map(run => ({
            ticks: Number(parameterText(run.params[slippageKey])),
            sharpe: run.metrics.sharpe,
            totalReturn: run.metrics.total_return,
          })),
      )
    : null;

  // Parameter sweep overfitting check: Deflated Sharpe Ratio & N-D grid neighbor median around best Sharpe.
  const sweepCheck = (() => {
    if (pureMultiSymbol || isSlippage) return null;
    const sharpeKey = runs.some(r => "development_sharpe" in r.metrics)
      ? "development_sharpe"
      : "sharpe";
    const sharpes = runs
      .map(r => r.metrics[sharpeKey])
      .filter((v): v is number => v !== undefined && Number.isFinite(v));
    if (sharpes.length < 2) return null;
    const bestSharpe = Math.max(...sharpes);
    const sampleRun = runs.find(r => r.firstDay && r.lastDay);
    const daysEstimate = sampleRun
      ? Math.max(
          20,
          Math.round(
            ((Date.parse(sampleRun.lastDay) - Date.parse(sampleRun.firstDay)) / 86400000) *
              (252 / 365),
          ),
        )
      : 252;
    const dsr = deflatedSharpeRatio(bestSharpe, sharpes, daysEstimate);

    let neighborMedian: number | null = null;
    if (sweptAxes.length >= 1) {
      let bestIdx = -1;
      let bestVal = -Infinity;
      baseRows.forEach((row, idx) => {
        const v = row.metrics[sharpeKey];
        if (v !== undefined && Number.isFinite(v) && v > bestVal) {
          bestVal = v;
          bestIdx = idx;
        }
      });
      if (bestIdx >= 0) {
        neighborMedian = medianValue(
          neighborMetricsOf(bestIdx, j => baseRows[j]!.metrics[sharpeKey]),
        );
      }
    }
    const plateauRatio =
      neighborMedian !== null && bestSharpe > 0 ? neighborMedian / bestSharpe : null;
    return { bestSharpe, dsr, neighborMedian, plateauRatio };
  })();

  return (
    <div className="lab-result">
      {slippageSummary && (
        <dl className="lab-figures" aria-label={t("滑点压力测试")}>
          <div>
            <dt>{t("盈亏平衡滑点")}</dt>
            <dd>
              {slippageSummary.status === "above_max"
                ? t("> {ticks} 跳", { ticks: slippageSummary.maxTicks })
                : slippageSummary.status === "interpolated" && slippageSummary.ticks !== null
                  ? t("{ticks} 跳", { ticks: slippageSummary.ticks.toFixed(1) })
                  : t("无正收益")}
            </dd>
          </div>
          {runs
            .slice()
            .sort(
              (a, b) =>
                Number(parameterText(a.params[slippageKey])) -
                Number(parameterText(b.params[slippageKey])),
            )
            .map(run => (
              <div key={run.id}>
                <dt>{t("{ticks} 跳", { ticks: parameterText(run.params[slippageKey]) })}</dt>
                <dd className={signed(run.metrics.total_return)}>
                  {run.metrics.sharpe !== undefined
                    ? metric(run.metrics.sharpe, false)
                    : metric(run.metrics.total_return, true)}
                </dd>
              </div>
            ))}
        </dl>
      )}
      {sweepCheck && (
        <dl className="lab-figures" aria-label={t("过拟合体检")}>
          <div>
            <dt>{t("最佳夏普")}</dt>
            <dd>{metric(sweepCheck.bestSharpe, false)}</dd>
          </div>
          <div>
            <dt>{t("检验组数")}</dt>
            <dd>{sweepCheck.dsr?.trials ?? runs.length}</dd>
          </div>
          <div>
            <dt>{t("DSR 置信度")}</dt>
            <dd>{metric(sweepCheck.dsr?.dsr, true)}</dd>
          </div>
          <div>
            <dt>{t("邻域中位")}</dt>
            <dd>{metric(sweepCheck.neighborMedian ?? undefined, false)}</dd>
          </div>
          <div>
            <dt>{t("高原稳健度")}</dt>
            <dd>{metric(sweepCheck.plateauRatio ?? undefined, true)}</dd>
          </div>
          {pbo !== undefined && Number.isFinite(pbo) && (
            <div>
              <dt>{t("PBO 过拟合概率")}</dt>
              <dd>{metric(pbo, true)}</dd>
            </div>
          )}
          {pboRankCorr !== undefined && Number.isFinite(pboRankCorr) && (
            <div>
              <dt>{t("CSCV 秩相关")}</dt>
              <dd className={signed(pboRankCorr)}>{metric(pboRankCorr, false)}</dd>
            </div>
          )}
        </dl>
      )}
      <div className="lab-toolbar">
        <label className="lab-inline">
          {t("按指标")}
          <select value={measure} onChange={event => setMeasure(event.target.value)}>
            {[...new Set(runs.flatMap(run => Object.keys(run.metrics)))].sort().map(name => (
              <option key={name} value={name}>
                {metricLabel(name)}
              </option>
            ))}
          </select>
        </label>
        {hasPlateau && (
          <label className="lab-inline">
            {t("排序依据")}
            <select
              value={sortBy}
              onChange={event => setSortBy(event.target.value as "metric" | "plateau")}
            >
              <option value="metric">{t("当前指标")}</option>
              <option value="plateau">{t("邻域高原")}</option>
            </select>
          </label>
        )}
        {sweptAxes.length >= 3 && (
          <>
            <label className="lab-inline">
              {t("行轴")}
              <select
                value={rowAxis}
                onChange={event => {
                  const next = event.target.value;
                  setRowAxisChoice(next);
                  if (next === colAxis) setColAxisChoice(rowAxis);
                }}
              >
                {sweptAxes.map(axis => (
                  <option key={axis} value={axis}>
                    {axisLabel(axis)}
                  </option>
                ))}
              </select>
            </label>
            <label className="lab-inline">
              {t("列轴")}
              <select
                value={colAxis}
                onChange={event => {
                  const next = event.target.value;
                  setColAxisChoice(next);
                  if (next === rowAxis) setRowAxisChoice(colAxis);
                }}
              >
                {sweptAxes.map(axis => (
                  <option key={axis} value={axis}>
                    {axisLabel(axis)}
                  </option>
                ))}
              </select>
            </label>
          </>
        )}
        {rolling && (
          <button type="button" className="lab-link" onClick={rolling}>
            {t("查看滚动验证的账户")}
          </button>
        )}
      </div>
      {grid && (
        <div className="lab-scroll" tabIndex={0} role="region" aria-label={t("参数热力图")}>
          <table className="lab-heatmap" aria-label={t("参数热力图")}>
            <thead>
              <tr>
                <th scope="col">
                  {axisLabel(rowAxis)} \ {axisLabel(colAxis)}
                </th>
                {grid.columns.map(column => (
                  <th scope="col" key={column}>
                    {column}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {grid.rows.map((row, r) => (
                <tr key={row}>
                  <th scope="row">{row}</th>
                  {grid.cells[r].map((cell, c) => {
                    const column = grid.columns[c]!;
                    const match = cellRuns?.get(`${row}\0${column}`);
                    const text = cell === null ? "—" : metric(cell, percent);
                    const ratio =
                      cell === null || high === low
                        ? null
                        : lowerBetter
                          ? (high - cell) / (high - low)
                          : (cell - low) / (high - low);
                    return (
                      <td
                        key={c}
                        className={match?.id === selected ? "selected" : undefined}
                        style={
                          ratio === null
                            ? undefined
                            : {
                                background: `color-mix(in oklab, var(--accent) ${Math.round(12 + 56 * ratio)}%, var(--panel))`,
                              }
                        }
                      >
                        {match?.state === "succeeded" ? (
                          <button
                            type="button"
                            className="lab-heatmap-cell"
                            title={`${axisLabel(rowAxis)}=${row}, ${axisLabel(colAxis)}=${column} · ${metricLabel(measure)}: ${text}`}
                            onClick={() => open(match.id)}
                          >
                            {text}
                          </button>
                        ) : (
                          text
                        )}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <VirtualScrollTable
        label={t("排名")}
        columns={
          1 +
          (hasSymbols ? 1 : 0) +
          names.length +
          (pureMultiSymbol ? 1 : 0) +
          3 +
          (extraMeasure ? 1 : 0) +
          (hasPlateau ? 1 : 0) +
          1
        }
        headers={
          <tr>
            <th scope="col">{t("排名")}</th>
            {hasSymbols && <th scope="col">{t("品种")}</th>}
            {names.map(name => (
              <th scope="col" key={name}>
                {axisLabel(name)}
              </th>
            ))}
            {pureMultiSymbol && <th scope="col">{t("区间")}</th>}
            <th scope="col" className="numeric">
              {metricLabel("total_return")}
            </th>
            <th scope="col" className="numeric">
              {metricLabel("sharpe")}
            </th>
            <th scope="col" className="numeric">
              {metricLabel("max_drawdown")}
            </th>
            {extraMeasure && (
              <th scope="col" className="numeric">
                {metricLabel(measure)}
              </th>
            )}
            {hasPlateau && (
              <th scope="col" className="numeric">
                {t("邻域中位")}
              </th>
            )}
            <th scope="col">{t("状态")}</th>
          </tr>
        }
        rows={ranked}
        renderRow={(row, index) => (
          <tr
            key={row.id}
            className={row.id === selected ? "selected" : undefined}
            data-openable={row.state === "succeeded" || undefined}
            onClick={row.state === "succeeded" ? () => open(row.id) : undefined}
          >
            <td>{index + 1}</td>
            {hasSymbols && <td>{row.symbols}</td>}
            {names.map(name => (
              <td key={name}>{row.params[name]}</td>
            ))}
            {pureMultiSymbol && <td className="subtle">{row.span || "—"}</td>}
            <td className={`numeric ${signed(row.metrics.total_return)}`}>
              {metric(row.metrics.total_return, true)}
            </td>
            <td className="numeric">{metric(row.metrics.sharpe, false)}</td>
            <td className="numeric">{metric(row.metrics.max_drawdown, true)}</td>
            {extraMeasure && (
              <td className="numeric">{row.metric === null ? "—" : metric(row.metric, percent)}</td>
            )}
            {hasPlateau && (
              <td className="numeric">
                {row.plateau === null ? "—" : metric(row.plateau, percent)}
              </td>
            )}
            <td>
              {row.state === "succeeded" ? (
                <button
                  type="button"
                  className="lab-link"
                  onClick={event => {
                    event.stopPropagation();
                    open(row.id);
                  }}
                >
                  {t("查看")}
                </button>
              ) : (
                t(states[row.state] ?? "未开始")
              )}
              {row.id === selected ? ` · ${t("选中")}` : ""}
            </td>
          </tr>
        )}
      />
    </div>
  );
}
