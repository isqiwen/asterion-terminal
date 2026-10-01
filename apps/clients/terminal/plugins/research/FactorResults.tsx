import { translate } from "../contract";
import { timestamp, type ResearchResult } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
const t = (key: string) => translate("asterion.terminal.research", key);
const correlation = (value: number | null) => (value === null ? t("未定义") : value.toFixed(4));
export function FactorResults({
  evidence,
}: {
  evidence: Extract<ResearchResult, { kind: "factor" }>;
}) {
  const { id, result } = evidence;
  return (
    <section className="research-result" aria-label={t("因子结果")}>
      <h3>{t("因子结果")}</h3>
      <p className="subtle">
        {t(
          result.folds.length
            ? "滚动验证"
            : result.partitions.length === 2
              ? "时间留出评价"
              : "按 K 线计算 · 样本内评价",
        )}
      </p>
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
                  <th>{t("回看 K 线数")}</th>
                  <th>{t("有效样本")}</th>
                  <th>Pearson</th>
                  <th>Spearman</th>
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
                  {t("前段")}: {fold.development.sample_count} · Spearman{" "}
                  {correlation(fold.development.spearman)}
                </p>
                {fold.candidates.map(c => (
                  <p key={c.lookback}>
                    {t("回看 K 线数")} {c.lookback} · Spearman {correlation(c.development_spearman)}
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
            {t("选中回看 K 线数")}: <strong>{result.lookback}</strong>
          </p>
          <details>
            <summary>{t("候选比较")}</summary>
            <p className="subtle">{t("仅按前段 Spearman 绝对值选择；完全相同时取较小窗口。")}</p>
            <div className="research-table">
              <table>
                <thead>
                  <tr>
                    <th>{t("回看 K 线数")}</th>
                    <th>{t("有效样本")}</th>
                    <th>Spearman</th>
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
        <section
          key={partition.name}
          aria-label={t(
            partition.name === "holdout"
              ? "留出段"
              : partition.name === "development"
                ? "前段"
                : "全样本",
          )}
        >
          <h4>
            {t(
              partition.name === "holdout"
                ? "留出段"
                : partition.name === "development"
                  ? "前段"
                  : "全样本",
            )}
          </h4>
          <div className="research-metrics">
            {[
              [t("有效样本"), partition.sample_count],
              ["Pearson", correlation(partition.pearson)],
              ["Spearman", correlation(partition.spearman)],
            ].map(([label, value]) => (
              <div key={label}>
                <span className="subtle">{label}</span>
                <strong>{value}</strong>
              </div>
            ))}
          </div>
          {(partition.pearson === null || partition.spearman === null) && (
            <p className="subtle">{t("因子或收益没有变化，相关性未定义。")}</p>
          )}
        </section>
      ))}
      <div className="research-range">
        <span>{timestamp(result.samples[0]?.timestamp_ns ?? null)}</span>
        <span>{timestamp(result.samples.at(-1)?.label_timestamp_ns ?? null)}</span>
      </div>
      <details>
        <summary>{t("样本明细（前 100 条）")}</summary>
        <div className="research-table">
          <table>
            <thead>
              <tr>
                <th>{t("因子时间")}</th>
                <th>{t("标签结束时间")}</th>
                <th>{t("动量")}</th>
                <th>{t("未来收益")}</th>
              </tr>
            </thead>
            <tbody>
              {result.samples.slice(0, 100).map(sample => (
                <tr key={sample.event_index}>
                  <td>{timestamp(sample.timestamp_ns)}</td>
                  <td>{timestamp(sample.label_timestamp_ns)}</td>
                  <td>{(sample.value * 100).toFixed(4)}%</td>
                  <td>{(sample.forward_return * 100).toFixed(4)}%</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </details>
      <ExperimentDetails evidence={evidence} />
      <details>
        <summary>{t("结果详情")}</summary>
        <p>{id}</p>
        <p>
          {t("共同预热 K 线数")}: {result.evaluation_warmup}
        </p>
        <p>
          {t("输入 K 线")}: {result.input_count} · {t("剔除跨界标签")}: {result.purged_count}
        </p>
        {result.partitions.map(p => (
          <p key={p.name}>
            {t(p.name === "holdout" ? "留出段" : p.name === "development" ? "前段" : "全样本")}:{" "}
            {p.begin_index + 1}–{p.end_index}
          </p>
        ))}
        <p>
          {!result.folds.length && (
            <>
              {t("回看 K 线数")}: {result.lookback} ·{" "}
            </>
          )}
          {t("未来收益 K 线数")}: {result.horizon}
        </p>
        <p>
          {t("数据版本")} <code>{result.dataset_revision}</code>
        </p>
        <p>{result.engine_version}</p>
      </details>
    </section>
  );
}
