import { useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type ResearchModelPlan,
  type ResearchModelTimeFold,
  type ResearchModelTimePlan,
  type ResearchModelTimeSpecification,
  type TaskResult,
  type ResourceObject,
} from "../../src/bridge/client";
import {
  asDisplayError,
  ErrorNotice,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { states, t, useRun } from "./shared";
import { ResearchModelMappings } from "./ResearchModelMappings";
type Report = Extract<TaskResult, { kind: "model_time_validation" }>;
type Fold = { [K in keyof ResearchModelTimeFold]: number | null };
type Draft = {
  recipe: ResearchModelPlan | null;
  name: string;
  purpose: string;
  limitations: string;
  folds: Fold[];
  runs: number;
  seconds: number;
  until: number;
};
const emptyFold = (): Fold => ({
  training_begin: null,
  training_end: null,
  validation_begin: null,
  validation_end: null,
});
const fresh = (): Draft => ({
  recipe: null,
  name: "",
  purpose: "",
  limitations: "",
  folds: [],
  runs: 0,
  seconds: 0,
  until: 0,
});
const reference = (v: ResourceObject) => [v.node, v.service, v.domain, v.kind, v.id, v.version];
const key = (s: ResearchModelTimeSpecification) =>
  JSON.stringify([
    s.name,
    s.purpose,
    s.limitations,
    reference(s.recipe),
    s.folds.map(f => [f.training_begin, f.training_end, f.validation_begin, f.validation_end]),
    s.run_budget,
    s.time_budget_seconds,
    s.valid_until_ms,
  ]);
const same = (a: ResourceObject, b: ResourceObject) =>
  JSON.stringify(reference(a)) === JSON.stringify(reference(b));
const uint = (n: number | null, max = 200000): n is number =>
  n !== null && Number.isInteger(n) && n >= 0 && n <= max;
export function ResearchModelTimeValidation({
  snapshot,
  query,
  trade,
  busy,
  recipe,
}: Pick<TerminalContext, "snapshot" | "query" | "trade" | "busy"> & {
  recipe: ResearchModelPlan | null;
}) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([service?.node_id, service?.owner_node_id, service?.service]);
  const current = useRef(destination);
  current.current = destination;
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`model-time-editor:${destination}`, fresh);
  const [selected, setSelected] = useWorkspaceDraft<ResearchModelTimePlan | null>(
    `model-time-fixed:${destination}`,
    null,
  );
  const [taskId, setTaskId] = useWorkspaceDraft(
    `model-time-task:${destination}`,
    () => `model-time-${crypto.randomUUID()}`,
  );
  const [plans, setPlans] = useState<ResearchModelTimePlan[]>([]),
    [after, setAfter] = useState("");
  const [report, setReport] = useState<Report | null>(null),
    [foldIndex, setFoldIndex] = useState(0),
    [page, setPage] = useState(0),
    [loading, setLoading] = useState(false);
  const { error, setError, run } = useRun(trade);
  const complete =
    draft.recipe &&
    draft.folds.length > 0 &&
    draft.folds.every(f => Object.values(f).every(n => uint(n)));
  const specification: ResearchModelTimeSpecification | null = complete
    ? {
        name: draft.name,
        purpose: draft.purpose,
        limitations: draft.limitations,
        recipe: draft.recipe!.object,
        folds: draft.folds.map(f => ({
          training_begin: f.training_begin!,
          training_end: f.training_end!,
          validation_begin: f.validation_begin!,
          validation_end: f.validation_end!,
        })),
        run_budget: draft.runs,
        time_budget_seconds: draft.seconds,
        valid_until_ms: draft.until,
      }
    : null;
  const [saveId, setSaveId] = useWorkspaceRequestId(
    `model-time-save:${destination}`,
    JSON.stringify(draft),
  );
  const dirty =
    Boolean(draft.recipe) &&
    (!selected || !specification || key(selected.specification) !== key(specification));
  useWorkspaceUnsaved(`model-time-editor:${destination}`, dirty);
  const blocked = busy || loading || !service?.online;
  const owns = (p: ResearchModelPlan) =>
    p.object.node === service?.owner_node_id && p.object.service === service?.service;
  const valid =
    specification &&
    draft.recipe &&
    owns(draft.recipe) &&
    draft.name.trim() &&
    draft.purpose.trim() &&
    draft.limitations.trim() &&
    draft.folds.length <= 32 &&
    uint(draft.runs, 4294967295) &&
    draft.runs >= draft.folds.length &&
    uint(draft.seconds, 4294967295) &&
    draft.seconds > 0 &&
    draft.until > Date.now() &&
    draft.until <= draft.recipe.specification.valid_until_ms &&
    draft.folds.every(
      (f, i) =>
        f.training_begin! < f.training_end! &&
        f.training_end! <= f.validation_begin! &&
        f.validation_begin! < f.validation_end! &&
        f.validation_end! <= draft.recipe!.source.holdout_start &&
        (!i || f.validation_begin! >= draft.folds[i - 1].validation_end!),
    ) &&
    draft.folds[0].training_begin === draft.recipe.specification.training_begin &&
    draft.folds[0].training_end === draft.recipe.specification.training_end;
  useEffect(() => {
    setPlans([]);
    setAfter("");
    setReport(null);
    setFoldIndex(0);
    setPage(0);
  }, [destination]);
  async function perform(action: () => Promise<void>) {
    setLoading(true);
    setError("");
    try {
      await action();
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setLoading(false);
    }
  }
  function edit<K extends keyof Draft>(field: K, value: Draft[K]) {
    setDraft(old => ({ ...old, [field]: value }));
    setSelected(null);
    setReport(null);
  }
  function choose(value: ResearchModelTimePlan) {
    setSelected(value);
    setDraft({
      recipe: value.recipe,
      name: value.specification.name,
      purpose: value.specification.purpose,
      limitations: value.specification.limitations,
      folds: value.specification.folds,
      runs: value.specification.run_budget,
      seconds: value.specification.time_budget_seconds,
      until: value.specification.valid_until_ms,
    });
    const existing = service?.tasks.find(
      task =>
        task.kind === "model_time_validation" &&
        task.model_time_plan &&
        same(task.model_time_plan, value.object),
    );
    setTaskId(existing?.id ?? `model-time-${crypto.randomUUID()}`);
    setReport(null);
    setFoldIndex(0);
    setPage(0);
  }
  async function list(cursor = "") {
    const owner = current.current;
    const value = (
      await query("research.model.list", { kind: "time_plan", after: cursor, limit: 30 })
    ).research_model;
    if (owner !== current.current) return;
    if (!value) throw new Error(t("模型响应缺失"));
    setPlans(old => (cursor ? [...old, ...value.time_plans] : value.time_plans));
    setAfter(value.next_after);
  }
  async function load(value: ResearchModelTimePlan) {
    if (dirty && !window.confirm(t("放弃当前未保存的研究草稿？"))) return;
    const owner = current.current;
    const found = (await query("research.model.read", { kind: "time_plan", id: value.object.id }))
      .research_model?.time_plan;
    if (owner !== current.current) return;
    if (!found || !same(found.object, value.object))
      throw new Error(t("逐折计划确认与原声明不一致"));
    choose(found);
  }
  async function save() {
    if (!specification || !valid) return;
    const owner = current.current,
      submitted = specification,
      id = saveId ?? `model-time-plan-${crypto.randomUUID()}`;
    setSaveId(id);
    const planId = `${id}-fixed`;
    if (!(await run("research.model.time.save", { id, plan_id: planId, specification: submitted })))
      return;
    const value = (await query("research.model.read", { kind: "time_plan", id: planId }))
      .research_model?.time_plan;
    if (owner !== current.current) return;
    if (!value || value.object.id !== planId || key(value.specification) !== key(submitted))
      throw new Error(t("逐折计划确认与原声明不一致"));
    choose(value);
    setSaveId(null);
    await list();
  }
  async function result() {
    if (!selected || !service) return;
    const owner = current.current;
    const value = (
      await query("task.result.read", { owner: taskOwner(service, "research"), id: taskId })
    ).task_result;
    if (owner !== current.current) return;
    if (
      !value ||
      value.kind !== "model_time_validation" ||
      value.id !== taskId ||
      !same(value.experiment.plan.object, selected.object)
    )
      throw new Error(t("逐折结果与固定计划不一致"));
    setReport(value);
    setFoldIndex(0);
    setPage(0);
  }
  const task = service?.tasks.find(value => value.id === taskId);
  const exclusions = {
    included: "参与开发验证",
    warmup: "预热",
    feature_unavailable: "特征缺失",
    label_unavailable: "标签缺失",
    crossed_label: "标签跨越验证截止",
    non_finite_prediction: "预测数值不可用",
  } as const;
  const labels = {
    training_begin: "训练起始索引",
    training_end: "训练截止索引",
    validation_begin: "验证起始索引",
    validation_end: "验证截止索引",
  } as const;
  const active = report?.result.folds[foldIndex];
  const boundary = (index: number) => {
    const value = selected!.boundaries[index];
    const utc =
      value.timestamp_ns === null
        ? t("来源未提供UTC时间")
        : `${new Date(Number(BigInt(value.timestamp_ns) / 1000000n)).toISOString()} (${value.timestamp_ns} ns)`;
    return (
      <small>
        {t("原交易日")}: {value.trading_day} / {utc}
      </small>
    );
  };
  return (
    <section aria-label={t("逐折模型时间验证")}>
      <h3>{t("逐折模型时间验证")}</h3>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      <p>
        {t("每折仅拟合截止前已成熟标签，再预测其后的开发区间；历史可得时间未知，原留出不参与。")}
      </p>
      <button
        disabled={
          blocked || !recipe || !owns(recipe) || recipe.specification.valid_until_ms <= Date.now()
        }
        onClick={() => {
          if (!recipe || (dirty && !window.confirm(t("放弃当前未保存的研究草稿？")))) return;
          setDraft({
            ...fresh(),
            recipe,
            folds: [
              {
                training_begin: recipe.specification.training_begin,
                training_end: recipe.specification.training_end,
                validation_begin: null,
                validation_end: null,
              },
            ],
          });
          setSelected(null);
          setReport(null);
        }}
      >
        {t("从当前固定方案建立时间验证")}
      </button>
      {draft.recipe && (
        <>
          <p>
            {t("固定训练方案")}: {draft.recipe.object.id} / {draft.recipe.object.version}
          </p>
          <p>
            {t("原留出起始索引")}: {draft.recipe.source.holdout_start}
          </p>
          <label>
            {t("时间验证名称")}
            <input
              disabled={blocked}
              value={draft.name}
              onChange={e => edit("name", e.target.value)}
            />
          </label>
          <label>
            {t("时间验证用途")}
            <textarea
              disabled={blocked}
              value={draft.purpose}
              onChange={e => edit("purpose", e.target.value)}
            />
          </label>
          <label>
            {t("时间验证限制说明")}
            <textarea
              disabled={blocked}
              value={draft.limitations}
              onChange={e => edit("limitations", e.target.value)}
            />
          </label>
          <p>{t("范围均含起点不含终点；训练截止不晚于验证起点，验证区间不得重叠或进入原留出。")}</p>
          <table aria-label={t("事先固定折清单")}>
            <thead>
              <tr>
                <th>{t("折序号")}</th>
                {Object.values(labels).map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {draft.folds.map((fold, i) => (
                <tr key={i}>
                  <td>{i + 1}</td>
                  {(Object.keys(labels) as (keyof Fold)[]).map(field => (
                    <td key={field}>
                      <input
                        type="number"
                        min={0}
                        max={200000}
                        aria-label={`${t(labels[field])} ${i + 1}`}
                        disabled={
                          blocked ||
                          (i === 0 && (field === "training_begin" || field === "training_end"))
                        }
                        value={fold[field] ?? ""}
                        onChange={e =>
                          edit(
                            "folds",
                            draft.folds.map((old, j) =>
                              j === i
                                ? {
                                    ...old,
                                    [field]: e.target.value === "" ? null : Number(e.target.value),
                                  }
                                : old,
                            ),
                          )
                        }
                      />
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
          <button
            disabled={blocked || draft.folds.length >= 32}
            onClick={() => edit("folds", [...draft.folds, emptyFold()])}
          >
            {t("添加显式折")}
          </button>
          <button
            disabled={blocked || draft.folds.length <= 1}
            onClick={() => edit("folds", draft.folds.slice(0, -1))}
          >
            {t("移除末折")}
          </button>
          <label>
            {t("逐折拟合次数预算")}
            <input
              type="number"
              min={1}
              disabled={blocked}
              value={draft.runs || ""}
              onChange={e => edit("runs", Number(e.target.value))}
            />
          </label>
          <label>
            {t("逐折任务时间预算（秒）")}
            <input
              type="number"
              min={1}
              disabled={blocked}
              value={draft.seconds || ""}
              onChange={e => edit("seconds", Number(e.target.value))}
            />
          </label>
          <label>
            {t("时间验证截止（UTC）")}
            <input
              type="datetime-local"
              disabled={blocked}
              value={draft.until ? new Date(draft.until).toISOString().slice(0, 16) : ""}
              onChange={e =>
                edit("until", e.target.value ? Date.parse(`${e.target.value}:00Z`) : 0)
              }
            />
          </label>
          <p>{t("实际拟合次数等于折数；预算不足明确拒绝，不自动缩小范围。")}</p>
          <button disabled={blocked || !valid || !dirty} onClick={() => void perform(save)}>
            {t("固定逐折计划（不执行）")}
          </button>
        </>
      )}
      <button disabled={blocked} onClick={() => void perform(() => list())}>
        {t("刷新逐折计划")}
      </button>
      {plans.map(value => (
        <p key={value.object.id}>
          <button disabled={blocked} onClick={() => void perform(() => load(value))}>
            {value.specification.name} / {value.object.id}
          </button>
        </p>
      ))}
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多逐折计划")}
        </button>
      )}
      {selected && (
        <section aria-label={t("固定逐折确认")}>
          <h4>{t("固定逐折确认")}</h4>
          <p>
            {selected.object.id} / {selected.object.version}
          </p>
          <p>
            {t("实际拟合次数")}: {selected.specification.folds.length} / {t("逐折拟合次数预算")}:{" "}
            {selected.specification.run_budget} / {t("逐折任务时间预算（秒）")}:{" "}
            {selected.specification.time_budget_seconds}
          </p>
          <p>
            {t("实际训练程序")}: {selected.recipe.program_artifact}
          </p>
          <p>{t("边界为原行半开区间；原UTC与交易日是来源标签，不代表资料当时已可得。")}</p>
          <table>
            <thead>
              <tr>
                <th>{t("折序号")}</th>
                <th>{t("训练范围")}</th>
                <th>{t("开发验证范围")}</th>
                <th>{t("成熟训练成员数")}</th>
              </tr>
            </thead>
            <tbody>
              {selected.specification.folds.map((fold, i) => (
                <tr key={i}>
                  <td>{i + 1}</td>
                  <td>
                    [{fold.training_begin}, {fold.training_end})<div>{boundary(i * 4)}</div>
                    <div>{boundary(i * 4 + 1)}</div>
                  </td>
                  <td>
                    [{fold.validation_begin}, {fold.validation_end})<div>{boundary(i * 4 + 2)}</div>
                    <div>{boundary(i * 4 + 3)}</div>
                  </td>
                  <td>{selected.training_counts[i]}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <label>
            {t("逐折任务ID")}
            <input
              disabled={blocked}
              value={taskId}
              onChange={e => {
                setTaskId(e.target.value);
                setReport(null);
              }}
            />
          </label>
          <button
            disabled={blocked || !taskId || selected.specification.valid_until_ms <= Date.now()}
            onClick={() =>
              void perform(async () => {
                await run("research.model.time.train", { id: taskId, plan: selected.object });
              })
            }
          >
            {t("显式接纳逐折任务")}
          </button>
          {task && (
            <p>
              {t(states[task.state])} {task.completed}/{task.total}
            </p>
          )}
          {task?.error && (
            <p role="alert">
              <ErrorNotice error={asDisplayError(new Error(task.error))} />
            </p>
          )}
          <button
            disabled={
              blocked ||
              !task ||
              !(["queued", "running"] as const).some(state => state === task.state)
            }
            onClick={() =>
              void perform(async () => {
                if (service)
                  await run("task.action", {
                    owner: taskOwner(service, "research"),
                    id: taskId,
                    action: "cancel",
                  });
              })
            }
          >
            {t("取消逐折任务")}
          </button>
          <button
            disabled={blocked || task?.state !== "succeeded"}
            onClick={() => void perform(result)}
          >
            {t("读取原逐折报告")}
          </button>
        </section>
      )}
      {report && active && (
        <section aria-label={t("原逐折模型报告")}>
          <p>{t("仅开发验证；原历史可得时间未知，不能据此宣称独立留出、历史可交易或策略盈利。")}</p>
          <label>
            {t("查看原折")}
            <select
              value={foldIndex}
              disabled={blocked}
              onChange={e => {
                setFoldIndex(Number(e.target.value));
                setPage(0);
              }}
            >
              {report.result.folds.map((_, i) => (
                <option key={i} value={i}>
                  {i + 1}
                </option>
              ))}
            </select>
          </label>
          <p>
            {t("成熟训练成员数")}: {active.training_members.length} / {t("实际参与评价数")}:{" "}
            {active.evaluated}
          </p>
          <p>
            {t("原折系数与截距")}: {active.coefficient} / {active.intercept}
          </p>
          {active.standardization && (
            <p>
              {t("固定训练均值与样本尺度")}: {active.standardization.mean} /{" "}
              {active.standardization.sample_standard_deviation}
            </p>
          )}
          <p>
            {t("开发验证均方误差")}:{" "}
            {active.validation_mean_squared_error === null
              ? t("无合格评价成员")
              : active.validation_mean_squared_error}
          </p>
          <table>
            <thead>
              <tr>
                <th>{t("原行索引")}</th>
                <th>{t("诊断预测")}</th>
                <th>{t("评价资格")}</th>
              </tr>
            </thead>
            <tbody>
              {active.predictions.slice(page * 50, (page + 1) * 50).map(row => (
                <tr key={row.event_index}>
                  <td>{row.event_index}</td>
                  <td>{row.prediction === null ? t("不可用") : row.prediction}</td>
                  <td>{t(exclusions[row.exclusion])}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={page === 0} onClick={() => setPage(value => value - 1)}>
            {t("上一页")}
          </button>
          <button
            disabled={(page + 1) * 50 >= active.predictions.length}
            onClick={() => setPage(value => value + 1)}
          >
            {t("下一页")}
          </button>
        </section>
      )}
      <ResearchModelMappings
        kind="model_time_validation"
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={busy}
        model={report}
      />
    </section>
  );
}
