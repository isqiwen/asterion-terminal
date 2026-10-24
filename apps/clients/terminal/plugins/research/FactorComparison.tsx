import { taskOwner } from "../../src/bridge/client";
import { ComparisonExport } from "./ComparisonExport";
import { useEffect, useRef, useState } from "react";
import type { FactorResult, TaskResult } from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { factorLabel, variableUnit, namespace, t } from "./shared";
export type FactorEvidence = Extract<TaskResult, { kind: "factor" }>;
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
const daily = (value: FactorEvidence) => value.experiment.series.every(s => s.kind === "table");
type Partition = FactorResult["partitions"][number];
const boundaries = (p: Partition) => [p.name, p.begin_index, p.end_index];
const observations = (value: FactorEvidence) =>
  value.experiment.series.length > 1 ? value.result.cross_sections : value.result.samples;
export function factorComparisonReasons(base: FactorEvidence, value: FactorEvidence): string[] {
  const a = base.experiment,
    b = value.experiment;
  const reasons: string[] = [];
  if (!daily(base) || !daily(value)) reasons.push("仅支持日线表因子");
  if (a.evaluation.mode !== b.evaluation.mode) reasons.push("评价模式不同");
  const evaluation = (v: FactorEvidence) => [
    v.experiment.evaluation,
    v.result.input_count,
    v.result.partitions.map(boundaries),
    v.result.folds.map(f => [
      f.training_begin,
      f.training_end,
      f.validation_end,
      boundaries(f.development),
      boundaries(f.holdout),
    ]),
  ];
  if (!same(evaluation(base), evaluation(value))) reasons.push("训练或验证边界不同");
  if (base.result.evaluation_warmup !== value.result.evaluation_warmup)
    reasons.push("评价预热不同");
  if (a.horizon !== b.horizon) reasons.push("未来收益跨度不同");
  const ranges = (v: FactorEvidence) =>
    v.experiment.series.map(s =>
      s.kind === "table"
        ? [
            s.data.instrument,
            s.data.begin_day,
            s.data.end_day,
            s.data.daily_source,
            s.data.calendar_source,
          ]
        : s.kind,
    );
  if (!same(ranges(base), ranges(value))) reasons.push("标的、范围或数据来源不同");
  if (a.dataset_revision !== b.dataset_revision) reasons.push("固定数据内容不同");
  const snapshots = (v: FactorEvidence) =>
    v.experiment.series.map(s => (s.kind === "table" ? s.data.snapshot : ""));
  if (!same(snapshots(base), snapshots(value))) reasons.push("固定快照不同");
  if (!same(a.liquidity, b.liquidity)) reasons.push("资格筛选规则不同");
  const left = observations(base);
  const right = observations(value);
  if (
    left.length !== right.length ||
    left.some(
      (row, i) =>
        row.event_index !== right[i].event_index ||
        row.observed !== right[i].observed ||
        row.label !== right[i].label,
    )
  ) {
    reasons.push("实际评价观测不同");
  } else if (
    base.result.cross_sections.some((row, i) => {
      // Factor rankings may differ; comparable cohorts depend on identities.
      const members = value.result.cross_sections[i]?.members;
      return (
        !members ||
        !same(
          row.members.map(m => m.series_index).sort((x, y) => x - y),
          members.map(m => m.series_index).sort((x, y) => x - y),
        )
      );
    })
  ) {
    reasons.push("实际参与成员不同");
  }
  return reasons;
}
const decimal = (value: number | null) => (value === null ? t("未定义") : value.toFixed(4));
function partitionMetrics(value: FactorEvidence, p: Partition) {
  const cross = value.experiment.series.length > 1;
  const spread = p.spreads[0];
  const rows = observations(value).filter(
    row => row.event_index >= p.begin_index && row.event_index < p.end_index,
  );
  return [
    ["评价观测区间", `${p.begin_index + 1}–${p.end_index}`, "display_only"],
    [
      "实际评价日期（观测至标签）",
      rows.length ? `${rows[0].observed} – ${rows.at(-1)!.label}` : "—",
      rows.length ? "display_only" : "no_observations",
    ],
    ["有效样本 / 截面", String(p.sample_count), "defined"],
    ["Pearson / IC 均值", decimal(p.pearson), p.pearson === null ? "undefined" : "defined"],
    ["Spearman / 秩 IC 均值", decimal(p.spearman), p.spearman === null ? "undefined" : "defined"],
    [
      "秩 IC 信息比率",
      cross ? decimal(p.spearman_ratio) : "—",
      !cross ? "not_applicable" : p.spearman_ratio === null ? "undefined" : "defined",
    ],
    [
      "每侧 1 个的多空差",
      cross && spread?.observations
        ? `${((spread.highest - spread.lowest) * 100).toFixed(4)}% (${spread.observations})`
        : "—",
      !cross ? "not_applicable" : spread?.observations ? "defined" : "no_observations",
    ],
  ];
}
export function FactorComparison({
  snapshot,
  busy,
  query,
}: Pick<TerminalContext, "snapshot" | "busy" | "query">) {
  const [selected, setSelected] = useState<string[]>([]);
  const [loaded, setLoaded] = useState<FactorEvidence[]>([]);
  const [failures, setFailures] = useState<{ id: string; error: DisplayError }[]>([]);
  const [loading, setLoading] = useState(false);
  const generation = useRef({ value: 0 });
  const connection = snapshot?.research_service?.connection_id;
  useEffect(() => {
    const epoch = generation.current;
    epoch.value++;
    setSelected([]);
    setLoaded([]);
    setFailures([]);
    setLoading(false);
    return () => {
      epoch.value++;
    };
  }, [connection]);
  const tasks =
    snapshot?.research_service?.tasks.filter(
      task => task.kind === "factor" && task.state === "succeeded",
    ) ?? [];
  const table = useRef<HTMLTableElement>(null);
  const base = loaded[0];
  const reasons = loaded.map(value => factorComparisonReasons(base, value));
  async function compare() {
    const current = generation.current.value;
    setLoading(true);
    setLoaded([]);
    setFailures([]);
    const replies = await Promise.allSettled(
      selected.map(async id => {
        const reply = await query("task.result.read", {
          owner: taskOwner(snapshot?.research_service, "research"),
          id,
        });
        const value = reply.task_result;
        if (value?.kind !== "factor") throw new Error(t("所选任务没有因子结果"));
        return value;
      }),
    );
    if (current !== generation.current.value) return;
    setLoaded(replies.flatMap(reply => (reply.status === "fulfilled" ? [reply.value] : [])));
    setFailures(
      replies.flatMap((reply, index) =>
        reply.status === "rejected"
          ? [{ id: selected[index], error: asDisplayError(reply.reason) }]
          : [],
      ),
    );
    setLoading(false);
  }
  const row = (label: string, values: string[]) => (
    <tr key={label}>
      <th scope="row">{t(label)}</th>
      {values.map((value, i) => (
        <td key={loaded[i].id} data-value-state="display_only">
          {value}
          {i > 0 && value !== values[0] && <strong> · {t("不同")}</strong>}
        </td>
      ))}
    </tr>
  );
  const statistics = (
    label: string,
    select: (value: FactorEvidence) => Partition | undefined,
    foldIndex?: number,
  ) => (
    <tr key={label}>
      <th scope="row">{label}</th>
      {loaded.map(value => {
        const p = select(value);
        const fold = foldIndex === undefined ? undefined : value.result.folds[foldIndex];
        const metrics = p ? partitionMetrics(value, p) : [];
        if (fold) {
          metrics.unshift(
            ["训练观测区间", `${fold.training_begin + 1}–${fold.training_end}`, "display_only"],
            ["实际窗口", String(fold.lookback), "display_only"],
          );
        }
        return (
          <td key={value.id} data-value-state={p ? "metric_group" : "not_applicable"}>
            {p ? (
              <dl>
                {metrics.map(([name, text, state]) => (
                  <div key={name}>
                    <dt>{t(name)}</dt>
                    <dd data-value-state={state}>{text}</dd>
                  </div>
                ))}
              </dl>
            ) : (
              t("不适用")
            )}
          </td>
        );
      })}
    </tr>
  );
  return (
    <section className="research-result" aria-label={t("日线因子比较")}>
      <h3>{t("日线因子比较")}</h3>
      <p className="subtle">
        {t(
          "选择 2–3 个已完成实验，只读取固定结果。首列为对照；口径不同仅独立列示，不排序、不合并，也不重新选参。",
        )}
      </p>
      <fieldset disabled={busy || loading}>
        <legend>{t("选择比较实验")}</legend>
        {tasks.map(task => (
          <label key={task.id}>
            <input
              type="checkbox"
              checked={selected.includes(task.id)}
              disabled={selected.length === 3 && !selected.includes(task.id)}
              onChange={event => {
                setLoaded([]);
                setFailures([]);
                setSelected(
                  event.target.checked
                    ? [...selected, task.id]
                    : selected.filter(id => id !== task.id),
                );
              }}
            />
            {task.id} · {task.instrument}
            {task.metadata?.name ? ` · ${task.metadata.name}` : ""}
          </label>
        ))}
        <button disabled={selected.length < 2} onClick={() => void compare()}>
          {t("读取并比较")}
        </button>
      </fieldset>
      <p>
        {t("已选实验")}:{" "}
        {selected.map(id => (
          <code key={id}>{id} </code>
        ))}
      </p>
      <button
        disabled={loading || selected.length === 0}
        onClick={() => {
          setSelected([]);
          setLoaded([]);
          setFailures([]);
        }}
      >
        {t("清空比较选择")}
      </button>
      {failures.length > 0 && <p>{t("部分结果不可读，以下仅列示已读取结果。")}</p>}
      {loading && <p role="status">{t("正在读取固定结果")}</p>}
      {failures.map(value => (
        <div key={value.id}>
          <code>{value.id}</code>
          <ErrorNotice error={value.error} namespace={namespace} />
        </div>
      ))}
      {base && (
        <>
          {failures.length === 0 && loaded.length === selected.length && (
            <ComparisonExport
              key={loaded.map(v => v.id).join(":")}
              table={table}
              evidence={loaded}
              reasons={reasons}
              busy={busy || loading}
              query={query}
            />
          )}
          <p>
            {t(
              "日线因子按全样本、前段与留出段或逐轮验证分别列示。同口径要求评价边界、预热、实际观测和参与成员相同；不重算共同样本。滚动仅比较各轮验证统计，不合并相关系数，训练得分仍属各自选参证据。",
            )}
          </p>
          <p className="subtle">
            {t(
              "分组收益是因子标签差，不是扣费回测收益。反复查看留出或验证结果后调参不能证明样本外有效。",
            )}
          </p>
          <div className="research-table">
            <table ref={table} aria-label={t("因子实验对照")}>
              <thead>
                <tr>
                  <th>{t("项目")}</th>
                  {loaded.map(value => (
                    <th key={value.id}>
                      <code>{value.id}</code>
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {row(
                  "实验名称",
                  loaded.map(v => v.task.metadata?.name || "—"),
                )}
                {row(
                  "实验备注",
                  loaded.map(v => v.task.metadata?.note || "—"),
                )}
                {row(
                  "比较口径",
                  reasons.map(values => {
                    return values.length
                      ? t("仅独立列示") + ": " + values.map(reason => t(reason)).join("；")
                      : t("同口径对照");
                  }),
                )}
                {row(
                  "因子",
                  loaded.map(v => factorLabel(v.experiment.factor)),
                )}
                {row(
                  "原变量定义",
                  loaded.map(v =>
                    v.experiment.variable_definition
                      ? `${v.experiment.variable_definition.object.id} / ${v.experiment.variable_definition.object.version}`
                      : "—",
                  ),
                )}
                {row(
                  "输出单位",
                  loaded.map(v =>
                    v.experiment.variable_definition
                      ? variableUnit(v.experiment.variable_definition.price_power)
                      : t("比例"),
                  ),
                )}
                {row(
                  "候选窗口",
                  loaded.map(v => v.experiment.lookbacks.join(", ")),
                )}
                {row(
                  "实际窗口",
                  loaded.map(v =>
                    v.result.folds.length ? t("按轮次列示") : String(v.result.lookback),
                  ),
                )}
                {row(
                  "评价预热",
                  loaded.map(v => String(v.result.evaluation_warmup)),
                )}
                {row(
                  "选参规则",
                  loaded.map(v =>
                    t(
                      v.result.selection_rule.includes("development_abs_spearman")
                        ? "按前段秩相关绝对值选参"
                        : "固定窗口",
                    ),
                  ),
                )}
                {row(
                  "未来收益跨度",
                  loaded.map(v => String(v.experiment.horizon)),
                )}
                {row(
                  "评价模式",
                  loaded.map(v =>
                    t(
                      v.experiment.evaluation.mode === "full_sample"
                        ? "全样本评价"
                        : v.experiment.evaluation.mode === "holdout"
                          ? "时间留出评价"
                          : "滚动验证",
                    ),
                  ),
                )}
                {row(
                  "标的与范围",
                  loaded.map(v =>
                    v.experiment.series
                      .map(s =>
                        s.kind === "table"
                          ? `${s.data.instrument}: ${s.data.begin_day} – ${s.data.end_day}`
                          : s.kind,
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "固定快照",
                  loaded.map(v =>
                    v.experiment.series
                      .map(s => (s.kind === "table" ? s.data.snapshot : "—"))
                      .join("; "),
                  ),
                )}
                {row(
                  "数据内容摘要",
                  loaded.map(v => v.experiment.dataset_revision),
                )}
                {row(
                  "数据来源",
                  loaded.map(v =>
                    v.experiment.series
                      .map(s =>
                        s.kind === "table"
                          ? `${s.data.daily_source} / ${s.data.calendar_source}`
                          : "—",
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "资格筛选",
                  loaded.map(v =>
                    v.experiment.liquidity
                      ? `${t("此前连续历史")}: ${v.experiment.liquidity.minimum_history}; ${t("上一观测成交量")}: ${v.experiment.liquidity.minimum_volume}; ${t("上一观测持仓量")}: ${v.experiment.liquidity.minimum_open_interest}`
                      : t("未启用"),
                  ),
                )}
                {row(
                  "结果摘要",
                  loaded.map(v => v.task.result_digest),
                )}
                {row(
                  "统计口径",
                  loaded.map(v => t(v.experiment.series.length > 1 ? "截面相关" : "时间序列相关")),
                )}
                {(["full_sample", "development", "holdout"] as const).map(
                  name =>
                    loaded.some(v => v.result.partitions.some(p => p.name === name)) &&
                    statistics(
                      t(
                        name === "full_sample"
                          ? "全样本"
                          : name === "development"
                            ? "前段"
                            : "留出段",
                      ),
                      v => v.result.partitions.find(p => p.name === name),
                    ),
                )}
                {Array.from(
                  { length: Math.max(0, ...loaded.map(v => v.result.folds.length)) },
                  (_, index) =>
                    statistics(
                      t("第 {count} 轮验证", { count: index + 1 }),
                      v => v.result.folds[index]?.holdout,
                      index,
                    ),
                )}
              </tbody>
            </table>
          </div>
        </>
      )}
    </section>
  );
}
