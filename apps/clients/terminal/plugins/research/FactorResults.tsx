import { useState } from "react";
import { FactorMembership } from "./FactorMembership";
import { timestamp, type TaskResult } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
import { factorWords, t, factorNames } from "./shared";

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
const percent = (value: number) => `${(value * 100).toFixed(4)}%`;
const partitionName = (name: string) =>
  t(name === "holdout" ? "留出段" : name === "development" ? "前段" : "全样本");
const pageSize = 50;

/** One result page for every factor task, whatever its series is made of. */
export function FactorResults({ evidence }: { evidence: Extract<TaskResult, { kind: "factor" }> }) {
  const { id, result, experiment } = evidence;
  // A dominant series of bars is read bar by bar, like a contract; the
  // market-wide table is read by trading day.
  const kind = experiment.series[0].kind === "table" ? "tables" : "bars";
  const words = factorWords[kind];
  const cross = experiment.series.length > 1;
  const names = {
    ...statistics[cross ? "cross" : "series"],
    rows: experiment.liquidity ? "评价截面数" : statistics[cross ? "cross" : "series"].rows,
  };
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
        {t(words.computed)} · {t(factorNames[experiment.factor])} ·{" "}
        {t(
          result.folds.length
            ? "滚动验证"
            : result.partitions.length === 2
              ? "时间留出评价"
              : "全样本评价",
        )}
        {cross && <> · {t("{count} 个合约的截面", { count: experiment.series.length })}</>}
        {experiment.series.some(
          series =>
            series.kind === "dominant" || (series.kind === "table" && series.data.rolls.length),
        ) && <> · {t("主力连续")}</>}
      </p>
      <FactorMembership experiment={experiment} result={result} />
      {experiment.liquidity && (
        <p className="subtle">
          {t(
            "评价截面数按观测计数，包含成员不足或相关性未定义的截面。选参、IC 与分组收益仅使用有资格且特征、标签完整的成员；资格不足不删除预热价格。",
          )}
        </p>
      )}
      {cross && (
        <p className="subtle">
          {t(
            "每个观测上比较当时有数据的合约的因子值与未来收益（IC 为相关系数，秩 IC 为秩相关）。一个合约参与某个观测，要求它的回看窗口和未来收益都落在自己没有缺口的数据里；参与的合约不足 3 个时该观测不计 IC。信息比率是均值除以各截面的标准差。分组收益是每个观测上因子最高和最低的若干个合约的平均未来收益，再对合约数够分成两侧的观测取均值；因子相同的合约按输入顺序排。",
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
                  {cross && <th>{t("每侧 1 个的多空差")}</th>}
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
                    {cross && (
                      <td>
                        {percent(fold.holdout.spreads[0].highest - fold.holdout.spreads[0].lowest)}
                      </td>
                    )}
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
          {cross && (
            <div className="research-table">
              <table aria-label={`${partitionName(partition.name)} · ${t("分组收益")}`}>
                <thead>
                  <tr>
                    <th>{t("每侧合约数")}</th>
                    <th>{t("因子最高的一组")}</th>
                    <th>{t("因子最低的一组")}</th>
                    <th>{t("多空差")}</th>
                    <th>{t("参与的观测")}</th>
                  </tr>
                </thead>
                <tbody>
                  {partition.spreads.map((spread, index) => (
                    <tr key={index}>
                      <td>{index + 1}</td>
                      {/* No observation had this many contracts on each side. */}
                      <td>{spread.observations ? percent(spread.highest) : "—"}</td>
                      <td>{spread.observations ? percent(spread.lowest) : "—"}</td>
                      <td>{spread.observations ? percent(spread.highest - spread.lowest) : "—"}</td>
                      <td>{spread.observations}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
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
                <th>{cross ? "IC" : t(factorNames[experiment.factor])}</th>
                <th>{cross ? t("秩 IC") : t("未来收益")}</th>
                {cross && <th>{t("参与的合约数")}</th>}
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
                        <td>{section.series}</td>
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
          {t(cross ? "至少一个合约有数据的观测" : words.input)}: {result.input_count} ·{" "}
          {t(names.rows)}: {rows.length} · {t("剔除跨界标签")}: {result.purged_count}
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
      </details>
    </section>
  );
}
