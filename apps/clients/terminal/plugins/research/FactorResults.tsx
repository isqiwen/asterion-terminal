import { useState } from "react";
import { timestamp, type TaskResult } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
import { factorWords, t } from "./shared";

const correlation = (value: number | null) => (value === null ? t("未定义") : value.toFixed(4));
// What a result's statistics are called: one series is correlated over time,
// several against each other at every observation.
const statistics = {
  series: {
    pearson: "Pearson",
    spearman: "Spearman",
    rows: "有效样本",
    details: "样本明细",
    undefined: "因子或收益没有变化，相关性未定义。",
    selection: "仅按前段 Spearman 绝对值选择；完全相同时取较小窗口。",
  },
  cross: {
    pearson: "IC 均值",
    spearman: "秩 IC 均值",
    rows: "有效截面",
    details: "截面明细",
    undefined: "各合约的因子或收益在这些观测上没有差别，截面相关未定义。",
    selection: "仅按前段秩 IC 均值的绝对值选择；完全相同时取较小窗口。",
  },
} as const;
const partitionName = (name: string) =>
  t(name === "holdout" ? "留出段" : name === "development" ? "前段" : "全样本");
const pageSize = 50;

/** One result page for every factor task, whatever its series is made of. */
export function FactorResults({ evidence }: { evidence: Extract<TaskResult, { kind: "factor" }> }) {
  const { id, result, experiment } = evidence;
  // A dominant series is read bar by bar, like a contract.
  const kind = experiment.series[0].kind === "daily" ? "daily" : "bars";
  const words = factorWords[kind];
  const cross = experiment.series.length > 1;
  const names = statistics[cross ? "cross" : "series"];
  const rows = cross ? result.cross_sections : result.samples;
  // Bars are labelled with timestamps, daily closes with the provider's dates.
  const when = (value: string | undefined) =>
    kind === "bars" ? timestamp(value ?? null) : (value ?? "—");
  const [page, setPage] = useState(0);
  const last = Math.max(0, Math.ceil(rows.length / pageSize) - 1);
  const current = Math.min(page, last);
  return (
    <section className="research-result" aria-label={t("因子结果")}>
      <h3>{t("因子结果")}</h3>
      <p className="subtle">
        {t(words.computed)} ·{" "}
        {t(
          result.folds.length
            ? "滚动验证"
            : result.partitions.length === 2
              ? "时间留出评价"
              : "全样本评价",
        )}
        {cross && <> · {t("{count} 个合约的截面", { count: experiment.series.length })}</>}
        {experiment.series.some(series => series.kind === "dominant") && <> · {t("主力连续")}</>}
      </p>
      {cross && (
        <p className="subtle">
          {t(
            "每个观测上比较各合约的动量与未来收益（IC 为相关系数，秩 IC 为秩相关），只使用所有合约都有的观测；信息比率是均值除以各截面的标准差。",
          )}
        </p>
      )}
      {result.folds.length > 0 && (
        <>
          <p className="subtle">
            {t("每轮仅使用此前训练段选参；以下统计只覆盖该轮验证段，不合并成一个相关系数。")}
          </p>
          <div className="research-table">
            <table aria-label={t("滚动验证结果")}>
              <thead>
                <tr>
                  <th>{t("轮次")}</th>
                  <th>{t("训练区间")}</th>
                  <th>{t("验证区间")}</th>
                  <th>{t(words.lookback)}</th>
                  <th>{t(names.rows)}</th>
                  <th>{t(names.pearson)}</th>
                  <th>{t(names.spearman)}</th>
                </tr>
              </thead>
              <tbody>
                {result.folds.map((fold, index) => (
                  <tr key={fold.training_end}>
                    <td>{index + 1}</td>
                    <td>
                      {fold.training_begin + 1}–{fold.training_end}
                    </td>
                    <td>
                      {fold.training_end + 1}–{fold.validation_end}
                    </td>
                    <td>{fold.lookback}</td>
                    <td>{fold.holdout.sample_count}</td>
                    <td>{correlation(fold.holdout.pearson)}</td>
                    <td>{correlation(fold.holdout.spearman)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <details>
            <summary>{t("逐轮选参证据")}</summary>
            {result.folds.map((fold, index) => (
              <section key={fold.training_end}>
                <h4>
                  {t("轮次")} {index + 1}
                </h4>
                <p>
                  {t("前段")}: {fold.development.sample_count} · {t(names.spearman)}{" "}
                  {correlation(fold.development.spearman)}
                </p>
                {fold.candidates.map(c => (
                  <p key={c.lookback}>
                    {t(words.lookback)} {c.lookback} · {t(names.spearman)}{" "}
                    {correlation(c.development_spearman)}
                  </p>
                ))}
              </section>
            ))}
          </details>
        </>
      )}
      {result.selection_rule === "development_abs_spearman" && (
        <>
          <p>
            {t(words.selected)}: <strong>{result.lookback}</strong>
          </p>
          <details>
            <summary>{t("候选比较")}</summary>
            <p className="subtle">{t(names.selection)}</p>
            <div className="research-table">
              <table>
                <thead>
                  <tr>
                    <th>{t(words.lookback)}</th>
                    <th>{t(names.rows)}</th>
                    <th>{t(names.spearman)}</th>
                  </tr>
                </thead>
                <tbody>
                  {result.candidates.map(c => (
                    <tr key={c.lookback}>
                      <td>
                        {c.lookback}
                        {c.lookback === result.lookback ? ` · ${t("已选中")}` : ""}
                      </td>
                      <td>{c.sample_count}</td>
                      <td>{correlation(c.development_spearman)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </details>
        </>
      )}
      {result.partitions.map(partition => (
        <section key={partition.name} aria-label={partitionName(partition.name)}>
          <h4>{partitionName(partition.name)}</h4>
          <div className="research-metrics">
            {[
              [t(names.rows), partition.sample_count],
              [t(names.pearson), correlation(partition.pearson)],
              [t(names.spearman), correlation(partition.spearman)],
              ...(cross
                ? [
                    [t("IC 信息比率"), correlation(partition.pearson_ratio)],
                    [t("秩 IC 信息比率"), correlation(partition.spearman_ratio)],
                  ]
                : []),
            ].map(([label, value]) => (
              <div key={label}>
                <span className="subtle">{label}</span>
                <strong>{value}</strong>
              </div>
            ))}
          </div>
          {(partition.pearson === null || partition.spearman === null) && (
            <p className="subtle">{t(names.undefined)}</p>
          )}
        </section>
      ))}
      <div className="research-range">
        <span>{when(rows[0]?.observed)}</span>
        <span>{when(rows.at(-1)?.label)}</span>
      </div>
      <details>
        <summary>{t(names.details)}</summary>
        <div className="research-table">
          <table aria-label={t(names.details)}>
            <thead>
              <tr>
                <th>{t(words.observed)}</th>
                <th>{t(words.label)}</th>
                <th>{cross ? "IC" : t("动量")}</th>
                <th>{cross ? t("秩 IC") : t("未来收益")}</th>
              </tr>
            </thead>
            <tbody>
              {cross
                ? result.cross_sections
                    .slice(current * pageSize, current * pageSize + pageSize)
                    .map(section => (
                      <tr key={section.event_index}>
                        <td>{when(section.observed)}</td>
                        <td>{when(section.label)}</td>
                        <td>{correlation(section.pearson)}</td>
                        <td>{correlation(section.spearman)}</td>
                      </tr>
                    ))
                : result.samples
                    .slice(current * pageSize, current * pageSize + pageSize)
                    .map(sample => (
                      <tr key={sample.event_index}>
                        <td>{when(sample.observed)}</td>
                        <td>{when(sample.label)}</td>
                        <td>{(sample.value * 100).toFixed(4)}%</td>
                        <td>{(sample.forward_return * 100).toFixed(4)}%</td>
                      </tr>
                    ))}
            </tbody>
          </table>
        </div>
        <button disabled={current === 0} onClick={() => setPage(current - 1)}>
          {t("上一页")}
        </button>
        <span>
          {" "}
          {current + 1} / {last + 1}{" "}
        </span>
        <button disabled={current === last} onClick={() => setPage(current + 1)}>
          {t("下一页")}
        </button>
      </details>
      <ExperimentDetails evidence={evidence} />
      <details>
        <summary>{t("结果详情")}</summary>
        <p>{id}</p>
        <p>
          {t(words.warmup)}: {result.evaluation_warmup}
        </p>
        <p>
          {t(cross ? "各合约共有的观测" : words.input)}: {result.input_count} · {t(names.rows)}:{" "}
          {rows.length} · {t("剔除跨界标签")}: {result.purged_count}
        </p>
        {result.partitions.map(p => (
          <p key={p.name}>
            {partitionName(p.name)}: {p.begin_index + 1}–{p.end_index}
          </p>
        ))}
        <p>
          {!result.folds.length && (
            <>
              {t(words.lookback)}: {result.lookback} ·{" "}
            </>
          )}
          {t(words.horizon)}: {result.horizon}
        </p>
        <p>
          {t("数据版本")} <code>{result.dataset_revision}</code>
        </p>
        <p>{result.engine_version}</p>
      </details>
    </section>
  );
}
