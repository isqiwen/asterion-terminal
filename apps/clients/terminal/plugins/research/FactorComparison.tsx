import { ComparisonExport } from "./ComparisonExport";
import { useEffect, useRef, useState } from "react";
import type { TaskResult } from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { factorNames, namespace, t } from "./shared";
export type FactorEvidence = Extract<TaskResult, { kind: "factor" }>;
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
const daily = (value: FactorEvidence) => value.experiment.series.every(s => s.kind === "table");
export function factorComparisonReasons(base: FactorEvidence, value: FactorEvidence): string[] {
  const a = base.experiment,
    b = value.experiment;
  const reasons: string[] = [];
  if (!daily(base) || !daily(value)) reasons.push("仅支持日线表因子");
  if (a.evaluation.mode !== "full_sample" || b.evaluation.mode !== "full_sample")
    reasons.push("仅支持全样本评价");
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
  return reasons;
}
export function factorComparisonValueState(label: string, value: FactorEvidence) {
  const p = value.result.partitions.find(p => p.name === "full_sample");
  const metrics = [
    "有效样本 / 截面",
    "Pearson / IC 均值",
    "Spearman / 秩 IC 均值",
    "秩 IC 信息比率",
    "每侧 1 个的多空差",
  ];
  if (!metrics.includes(label)) return "display_only";
  if (!p) return value.experiment.evaluation.mode === "full_sample" ? "missing" : "not_applicable";
  if (label === "有效样本 / 截面") return "defined";
  if (label === "Pearson / IC 均值") return p.pearson === null ? "undefined" : "defined";
  if (label === "Spearman / 秩 IC 均值") return p.spearman === null ? "undefined" : "defined";
  if (value.experiment.series.length === 1) return "not_applicable";
  if (label === "秩 IC 信息比率") return p.spearman_ratio === null ? "undefined" : "defined";
  return p.spreads[0]?.observations ? "defined" : "no_observations";
}
const decimal = (value: number | null) => (value === null ? t("未定义") : value.toFixed(4));
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
  const connection = snapshot?.task_service?.connection_id;
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
    snapshot?.task_service?.tasks.filter(
      task => task.kind === "factor" && task.state === "succeeded",
    ) ?? [];
  const table = useRef<HTMLTableElement>(null);
  const base = loaded[0];
  async function compare() {
    const current = generation.current.value;
    setLoading(true);
    setLoaded([]);
    setFailures([]);
    const replies = await Promise.allSettled(
      selected.map(async id => {
        const reply = await query("task.result.read", { id });
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
        <td key={loaded[i].id} data-value-state={factorComparisonValueState(label, loaded[i])}>
          {value}
          {i > 0 && value !== values[0] && <strong> · {t("不同")}</strong>}
        </td>
      ))}
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
              reasons={loaded.map(v => [
                ...new Set([...factorComparisonReasons(v, v), ...factorComparisonReasons(base, v)]),
              ])}
              busy={busy || loading}
              query={query}
            />
          )}
          <p>
            {t(
              "仅支持日线全样本因子。有效样本可随窗口变化，未重算共同样本；分组收益是因子标签差，不是扣费回测收益。因子无交易费用、保证金或账户风险口径。",
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
                  loaded.map(value => {
                    const reasons = [
                      ...new Set([
                        ...factorComparisonReasons(value, value),
                        ...factorComparisonReasons(base, value),
                      ]),
                    ];
                    return reasons.length
                      ? t("仅独立列示") + ": " + reasons.map(reason => t(reason)).join("；")
                      : t("同口径对照");
                  }),
                )}
                {row(
                  "因子",
                  loaded.map(v => t(factorNames[v.experiment.factor])),
                )}
                {row(
                  "候选窗口",
                  loaded.map(v => v.experiment.lookbacks.join(", ")),
                )}
                {row(
                  "实际窗口",
                  loaded.map(v => String(v.result.lookback)),
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
                {row(
                  "有效样本 / 截面",
                  loaded.map(v =>
                    String(
                      v.result.partitions.find(p => p.name === "full_sample")?.sample_count ?? "—",
                    ),
                  ),
                )}
                {row(
                  "Pearson / IC 均值",
                  loaded.map(v => {
                    const p = v.result.partitions.find(p => p.name === "full_sample");
                    return p ? decimal(p.pearson) : "—";
                  }),
                )}
                {row(
                  "Spearman / 秩 IC 均值",
                  loaded.map(v => {
                    const p = v.result.partitions.find(p => p.name === "full_sample");
                    return p ? decimal(p.spearman) : "—";
                  }),
                )}
                {row(
                  "秩 IC 信息比率",
                  loaded.map(v => {
                    const p = v.result.partitions.find(p => p.name === "full_sample");
                    return v.experiment.series.length > 1 && p ? decimal(p.spearman_ratio) : "—";
                  }),
                )}
                {row(
                  "每侧 1 个的多空差",
                  loaded.map(v => {
                    const p = v.result.partitions.find(p => p.name === "full_sample");
                    const spread = p?.spreads[0];
                    return v.experiment.series.length > 1 && spread && spread.observations > 0
                      ? `${((spread.highest - spread.lowest) * 100).toFixed(4)}% (${spread.observations})`
                      : "—";
                  }),
                )}
              </tbody>
            </table>
          </div>
        </>
      )}
    </section>
  );
}
