import { useCallback, useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type ResearchModelDraft,
  type ResearchModelPlan,
  type ResearchModelSpecification,
  type TaskResult,
} from "../../src/bridge/client";
import {
  asDisplayError,
  ErrorNotice,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { ResearchPage, t, useRun } from "./shared";
import { ResearchModelMappings } from "./ResearchModelMappings";
import { ResearchModelTimeValidation } from "./ResearchModelTimeValidation";
type ModelReport = Extract<TaskResult, { kind: "model_training" }>;
const fresh = () => ({
  id: `model-draft-${crypto.randomUUID()}`,
  specification: {
    name: "",
    purpose: "",
    limitations: "",
    source: null,
    method: "unspecified",
    fit_intercept: true,
    alpha: null,
    training_begin: 0,
    training_end: 0,
    standardize: false,
    weights: "equal",
    run_budget: 0,
    time_budget_seconds: 0,
    valid_until_ms: 0,
    mode: "post_hoc_diagnostic",
  } satisfies ResearchModelSpecification,
});
const same = (a: { id: string; version: string }, b: { id: string; version: string }) =>
  a.id === b.id && a.version === b.version;
const canonicalAlpha = (value: string | null) =>
  value === null
    ? null
    : value
        .replace(/^\+/, "")
        .replace(/^(-?)0+(?=\d)/, "$1")
        .replace(/(\.\d*?)0+$/, "$1")
        .replace(/\.$/, "")
        .replace(/^-0$/, "0");
const specificationKey = (value: ResearchModelSpecification) =>
  JSON.stringify([
    value.name,
    value.purpose,
    value.limitations,
    value.source
      ? [
          value.source.node,
          value.source.service,
          value.source.domain,
          value.source.kind,
          value.source.id,
          value.source.version,
        ]
      : null,
    value.method,
    value.fit_intercept,
    canonicalAlpha(value.alpha),
    value.training_begin,
    value.training_end,
    value.standardize,
    value.weights,
    value.run_budget,
    value.time_budget_seconds,
    value.valid_until_ms,
    value.mode,
  ]);
const uint = (n: number, max = 200000) => Number.isInteger(n) && n >= 0 && n <= max;
export function ResearchModels({ snapshot, query, trade, busy }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([service?.node_id, service?.owner_node_id, service?.service]);
  const current = useRef(destination);
  current.current = destination;
  const ask = useRef(query);
  ask.current = query;
  const [draft, setDraft] = useWorkspaceDraft<{
    id: string;
    specification: ResearchModelSpecification;
  }>(`model-editor:${destination}`, fresh);
  const [saved, setSaved] = useWorkspaceDraft<ResearchModelDraft | null>(
    `model-saved:${destination}`,
    null,
  );
  const [plan, setPlan] = useWorkspaceDraft<ResearchModelPlan | null>(
    `model-plan:${destination}`,
    null,
  );
  const [taskId, setTaskId] = useWorkspaceDraft(
    `model-task:${destination}`,
    () => `model-training-${crypto.randomUUID()}`,
  );
  const [sourceId, setSourceId] = useWorkspaceDraft(`model-source:${destination}`, "");
  const [drafts, setDrafts] = useState<ResearchModelDraft[]>([]),
    [plans, setPlans] = useState<ResearchModelPlan[]>([]);
  const [draftAfter, setDraftAfter] = useState(""),
    [planAfter, setPlanAfter] = useState("");
  const [report, setReport] = useState<ModelReport | null>(null),
    [rowPage, setRowPage] = useState(0),
    [loading, setLoading] = useState(false);
  const { error, setError, run } = useRun(trade);
  const [saveId, setSaveId] = useWorkspaceRequestId(
    `model-save:${destination}`,
    JSON.stringify([draft, saved?.object.version]),
  );
  const [fixId, setFixId] = useWorkspaceRequestId(
    `model-fix:${destination}`,
    JSON.stringify(saved?.object ?? null),
  );
  const spec = draft.specification;
  const dirty = saved
    ? specificationKey(saved.specification) !== specificationKey(spec) ||
      saved.object.id !== draft.id
    : Boolean(spec.name || spec.purpose || spec.limitations || spec.source);
  useWorkspaceUnsaved(`model-editor:${destination}`, dirty);
  const blocked = busy || loading || !service?.online;
  const canFix =
    saved &&
    !dirty &&
    spec.source &&
    spec.purpose.trim() &&
    spec.limitations.trim() &&
    uint(spec.training_begin) &&
    uint(spec.training_end) &&
    spec.training_begin < spec.training_end &&
    uint(spec.run_budget, 4294967295) &&
    spec.run_budget > 0 &&
    uint(spec.time_budget_seconds, 4294967295) &&
    spec.time_budget_seconds > 0 &&
    spec.valid_until_ms > Date.now() &&
    spec.fit_intercept !== null &&
    ((spec.method === "linear" && spec.alpha === null) ||
      (spec.method === "ridge" && spec.alpha !== null));
  const list = useCallback(async (kind: "draft" | "plan", after = "") => {
    const owner = current.current;
    const value = (await ask.current("research.model.list", { kind, after, limit: 30 }))
      .research_model;
    if (owner !== current.current) return;
    if (!value) throw new Error(t("模型响应缺失"));
    if (kind === "draft") {
      setDrafts(old => (after ? [...old, ...value.drafts] : value.drafts));
      setDraftAfter(value.next_after);
    } else {
      setPlans(old => (after ? [...old, ...value.plans] : value.plans));
      setPlanAfter(value.next_after);
    }
  }, []);
  useEffect(() => {
    setDrafts([]);
    setPlans([]);
    setDraftAfter("");
    setPlanAfter("");
    setReport(null);
    setRowPage(0);
    const owner = destination;
    if (!service?.online) return;
    void Promise.all([list("draft"), list("plan")]).catch(reason => {
      if (current.current === owner) setError(asDisplayError(reason));
    });
  }, [destination, service?.online, list, setError]);
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
  function edit<K extends keyof ResearchModelSpecification>(
    key: K,
    value: ResearchModelSpecification[K],
  ) {
    setDraft(old => ({ ...old, specification: { ...old.specification, [key]: value } }));
    setPlan(null);
    setReport(null);
  }
  async function readSource() {
    if (!service) return;
    if (!service.owner_node_id) throw new Error(t("Research稳定业务身份不可用，不能固定来源"));
    const owner = current.current;
    const value = (
      await query("task.result.read", { owner: taskOwner(service, "research"), id: sourceId })
    ).task_result;
    if (owner !== current.current) return;
    if (!value || value.kind !== "factor" || value.id !== sourceId)
      throw new Error(t("模型来源需要原成功因子结果"));
    edit("source", {
      node: service.owner_node_id,
      service: service.service,
      domain: "research",
      kind: "factor_result",
      id: value.id,
      version: value.task.result_digest,
    });
  }
  async function save() {
    const owner = current.current,
      submitted = draft;
    const id = saveId ?? `model-save-${crypto.randomUUID()}`;
    setSaveId(id);
    if (
      !(await run("research.model.save", {
        id,
        draft_id: submitted.id,
        expected_digest: saved?.object.version ?? "",
        specification: submitted.specification,
      }))
    )
      return;
    const value = (await query("research.model.read", { kind: "draft", id: submitted.id }))
      .research_model?.draft;
    if (owner !== current.current) return;
    if (
      !value ||
      value.object.id !== submitted.id ||
      specificationKey(value.specification) !== specificationKey(submitted.specification)
    )
      throw new Error(t("模型确认与原声明不一致"));
    setSaved(value);
    setSaveId(null);
    await list("draft");
  }
  async function fix() {
    if (!saved) return;
    const owner = current.current;
    const id = fixId ?? `model-fix-${crypto.randomUUID()}`;
    setFixId(id);
    const planId = `model-plan-${id}`;
    if (
      !(await run("research.model.fix", {
        id,
        plan_id: planId,
        draft_id: saved.object.id,
        expected_digest: saved.object.version,
      }))
    )
      return;
    const value = (await query("research.model.read", { kind: "plan", id: planId })).research_model
      ?.plan;
    if (owner !== current.current) return;
    if (!value || value.draft_digest !== saved.object.version || value.draft_id !== saved.object.id)
      throw new Error(t("模型确认与原声明不一致"));
    setPlan(value);
    setFixId(null);
    setReport(null);
    setTaskId(`model-training-${crypto.randomUUID()}`);
    await list("plan");
  }
  async function load(kind: "draft" | "plan", id: string) {
    if (dirty && !window.confirm(t("放弃当前未保存的研究草稿？"))) return;
    const owner = current.current;
    const value = (await query("research.model.read", { kind, id })).research_model;
    if (owner !== current.current) return;
    if (!value) throw new Error(t("模型响应缺失"));
    if (kind === "draft") {
      if (!value.draft) throw new Error(t("模型响应缺失"));
      setSaved(value.draft);
      setDraft({ id: value.draft.object.id, specification: value.draft.specification });
      setPlan(null);
    } else {
      if (!value.plan) throw new Error(t("模型响应缺失"));
      setPlan(value.plan);
      const prior = service?.tasks.find(
        task =>
          task.kind === "model_training" &&
          task.model_plan &&
          same(task.model_plan, value.plan!.object),
      );
      setTaskId(prior?.id ?? `model-training-${crypto.randomUUID()}`);
    }
    setReport(null);
    setRowPage(0);
  }
  async function readResult() {
    if (!service || !plan) return;
    const owner = current.current;
    const value = (
      await query("task.result.read", { owner: taskOwner(service, "research"), id: taskId })
    ).task_result;
    if (owner !== current.current) return;
    if (
      !value ||
      value.kind !== "model_training" ||
      value.id !== taskId ||
      !same(value.experiment.plan.object, plan.object)
    )
      throw new Error(t("模型结果与固定方案不一致"));
    setReport(value);
    setRowPage(0);
  }
  const task = service?.tasks.find(value => value.id === taskId);
  const roles = {
    training: "训练范围内",
    other_development: "其它开发行",
    holdout: "原留出行",
  } as const;
  const reasons = {
    fitted: "参与拟合",
    not_training: "训练范围外",
    warmup: "预热",
    missing_feature: "特征缺失",
    missing_label: "标签缺失",
    crossed_label: "标签跨越训练截止",
  } as const;
  return (
    <ResearchPage title={t("固定来源模型训练")} taskService={service} error={error}>
      <p>
        {t(
          "仅支持单特征线性或岭回归。保存与固定不运行任务；事后诊断预测不代表历史可交易信号，也不授权交易。",
        )}
      </p>
      <button
        disabled={blocked}
        onClick={() => {
          if (!dirty || window.confirm(t("放弃当前未保存的研究草稿？"))) {
            setDraft(fresh());
            setSaved(null);
            setPlan(null);
            setReport(null);
          }
        }}
      >
        {t("新建模型草稿")}
      </button>
      <fieldset disabled={blocked}>
        <legend>{t("模型草稿")}</legend>
        <label>
          {t("模型名称")}
          <input value={spec.name} onChange={event => edit("name", event.target.value)} />
        </label>
        <label>
          {t("模型用途")}
          <textarea value={spec.purpose} onChange={event => edit("purpose", event.target.value)} />
        </label>
        <label>
          {t("模型限制说明")}
          <textarea
            value={spec.limitations}
            onChange={event => edit("limitations", event.target.value)}
          />
        </label>
        <label>
          {t("原因子任务ID")}
          <input
            value={sourceId}
            onChange={event => setSourceId(event.target.value)}
            list="model-factor-tasks"
          />
        </label>
        <datalist id="model-factor-tasks">
          {service?.tasks
            .filter(
              value => value.kind === "factor" && value.state === "succeeded" && !value.retired,
            )
            .map(value => (
              <option key={value.id} value={value.id} />
            ))}
        </datalist>
        <button disabled={!sourceId.trim()} onClick={() => void perform(readSource)}>
          {t("读取并固定原因子证据")}
        </button>
        {spec.source && (
          <p>
            {t("固定来源")}:{" "}
            <code>
              {spec.source.id} · {spec.source.version}
            </code>
          </p>
        )}
        <label>
          {t("回归方法")}
          <select
            value={spec.method}
            onChange={event => {
              const method =
                event.target.value === "ridge"
                  ? "ridge"
                  : event.target.value === "linear"
                    ? "linear"
                    : "unspecified";
              edit("method", method);
              if (method !== "ridge") edit("alpha", null);
            }}
          >
            <option value="unspecified">{t("请选择回归算法")}</option>
            <option value="linear">OLS</option>
            <option value="ridge">Ridge</option>
          </select>
        </label>
        <label>
          {t("拟合截距")}
          <input
            type="checkbox"
            checked={spec.fit_intercept === true}
            onChange={event => edit("fit_intercept", event.target.checked)}
          />
        </label>
        <label>
          {t("训练样本标准化（n−1）")}
          <input
            type="checkbox"
            checked={spec.standardize}
            onChange={event => edit("standardize", event.target.checked)}
          />
        </label>
        <label>
          {t("岭惩罚参数alpha")}
          <input
            disabled={spec.method !== "ridge"}
            value={spec.alpha ?? ""}
            onChange={event => edit("alpha", event.target.value || null)}
          />
        </label>
        <p>
          {t(
            "训练范围为原行索引左闭右开，只拟合标签在截止前成熟的训练成员，均值和尺度不读取留出。",
          )}
        </p>
        <label>
          {t("训练起始索引")}
          <input
            type="number"
            min="0"
            max="200000"
            step="1"
            value={spec.training_begin}
            onChange={event => edit("training_begin", Number(event.target.value))}
          />
        </label>
        <label>
          {t("训练截止索引（不含）")}
          <input
            type="number"
            min="0"
            max="200000"
            step="1"
            value={spec.training_end}
            onChange={event => edit("training_end", Number(event.target.value))}
          />
        </label>
        <label>
          {t("训练次数预算")}
          <input
            type="number"
            min="1"
            step="1"
            value={spec.run_budget || ""}
            onChange={event => edit("run_budget", Number(event.target.value))}
          />
        </label>
        <label>
          {t("训练时间预算（秒）")}
          <input
            type="number"
            min="1"
            step="1"
            value={spec.time_budget_seconds || ""}
            onChange={event => edit("time_budget_seconds", Number(event.target.value))}
          />
        </label>
        <label>
          {t("适用截止（UTC）")}
          <input
            type="datetime-local"
            value={
              spec.valid_until_ms ? new Date(spec.valid_until_ms).toISOString().slice(0, 16) : ""
            }
            onChange={event =>
              edit("valid_until_ms", event.target.value ? Date.parse(event.target.value + "Z") : 0)
            }
          />
        </label>
        <button disabled={!spec.name.trim()} onClick={() => void perform(save)}>
          {t("保存模型草稿")}
        </button>
        <button disabled={!canFix} onClick={() => void perform(fix)}>
          {t("固定模型训练方案")}
        </button>
      </fieldset>
      {saved && (
        <p>
          {t("已保存原草稿")}:{" "}
          <code>
            {saved.object.id} · {saved.object.version}
          </code>
          {dirty && ` · ${t("有未保存修改")}`}
        </p>
      )}
      <section aria-label={t("模型资产列表")}>
        <h3>{t("模型草稿")}</h3>
        {drafts.map(value => (
          <button
            key={value.object.id}
            disabled={blocked}
            onClick={() => void perform(() => load("draft", value.object.id))}
          >
            {value.specification.name} · {value.object.id}
          </button>
        ))}
        {draftAfter && (
          <button disabled={blocked} onClick={() => void perform(() => list("draft", draftAfter))}>
            {t("更多草稿")}
          </button>
        )}
        <h3>{t("固定训练方案")}</h3>
        {plans.map(value => (
          <button
            key={value.object.id}
            disabled={blocked}
            onClick={() => void perform(() => load("plan", value.object.id))}
          >
            {value.specification.name} · {value.object.id}
          </button>
        ))}
        {planAfter && (
          <button disabled={blocked} onClick={() => void perform(() => list("plan", planAfter))}>
            {t("更多固定方案")}
          </button>
        )}
      </section>
      {plan && (
        <section aria-label={t("固定模型训练方案")}>
          <h3>{plan.specification.name}</h3>
          <code>
            {plan.object.id} · {plan.object.version}
          </code>
          <p>
            {t("固定来源")}: {plan.source.result.id} · {plan.source.result.version}
          </p>
          <p>
            {t("实际成熟训练成员")}: {plan.training_samples} · [{plan.specification.training_begin},{" "}
            {plan.specification.training_end}) · {t("原留出起始索引")}: {plan.source.holdout_start}
          </p>
          <p>
            {t("单一参数候选")}: 1 · {t("回归方法")}:{" "}
            {plan.specification.method === "linear" ? "OLS" : "Ridge"}· alpha:{" "}
            {plan.specification.alpha ?? "—"} · {t("拟合截距")}:{" "}
            {plan.specification.fit_intercept ? t("是") : t("否")}· {t("训练样本标准化（n−1）")}:{" "}
            {plan.specification.standardize ? t("是") : t("否")}
          </p>
          <p>
            {t("训练次数预算")}: {plan.specification.run_budget} · {t("训练时间预算（秒）")}:{" "}
            {plan.specification.time_budget_seconds}· {t("适用截止（UTC）")}:{" "}
            {new Date(plan.specification.valid_until_ms).toISOString()}
          </p>
          <p>
            {t("原程序身份")}: <code>{plan.program_artifact}</code>
          </p>
          <p>{t("历史可得时间未知，仅事后诊断；预测不是仓位。")}</p>
          <label>
            {t("模型训练任务ID")}
            <input
              value={taskId}
              disabled={blocked}
              onChange={event => {
                setTaskId(event.target.value);
                setReport(null);
              }}
            />
          </label>
          <button
            disabled={blocked || !taskId.trim() || plan.specification.valid_until_ms <= Date.now()}
            onClick={() =>
              void perform(async () => {
                await run("research.model.train", { id: taskId, plan: plan.object });
              })
            }
          >
            {t("显式接纳训练任务")}
          </button>
          <p role="status">
            {task
              ? `${task.id} · ${task.state} · ${task.completed}/${task.total}`
              : t("尚未读到该任务接纳状态")}
          </p>
          {task?.error && <ErrorNotice error={asDisplayError(new Error(task.error))} />}
          <button
            disabled={
              blocked ||
              !service ||
              task?.kind !== "model_training" ||
              !task.model_plan ||
              !same(task.model_plan, plan.object) ||
              !["queued", "running", "publishing"].includes(task.state)
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
            {t("取消原训练任务")}
          </button>
          <button disabled={blocked || !taskId.trim()} onClick={() => void perform(readResult)}>
            {t("读取原模型结果")}
          </button>
        </section>
      )}
      {report && (
        <section aria-label={t("原模型训练结果")}>
          <p>
            {t("实际成熟训练成员")}: {report.result.training_samples} · {t("截距")}:{" "}
            {report.result.intercept} · {t("系数")}: {report.result.coefficient}
          </p>
          <p>
            SSE: {report.result.residual_sum_squares} · {t("正则项")}:{" "}
            {report.result.regularization} · {t("目标值")}: {report.result.objective}
          </p>
          {report.result.standardization && (
            <p>
              {t("固定训练均值与样本尺度")}: {report.result.standardization.mean} /{" "}
              {report.result.standardization.sample_standard_deviation} · n=
              {report.result.standardization.training_samples}
            </p>
          )}
          <table>
            <thead>
              <tr>
                <th>{t("原行索引")}</th>
                <th>{t("行角色")}</th>
                <th>{t("拟合资格")}</th>
                <th>{t("诊断预测")}</th>
              </tr>
            </thead>
            <tbody>
              {report.result.rows.slice(rowPage * 50, (rowPage + 1) * 50).map(row => (
                <tr key={row.event_index}>
                  <td>{row.event_index}</td>
                  <td>{t(roles[row.role])}</td>
                  <td>{t(reasons[row.reason])}</td>
                  <td>{row.prediction === null ? t("不可用") : row.prediction}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={rowPage === 0} onClick={() => setRowPage(value => value - 1)}>
            {t("上一页")}
          </button>
          <button
            disabled={(rowPage + 1) * 50 >= report.result.rows.length}
            onClick={() => setRowPage(value => value + 1)}
          >
            {t("下一页")}
          </button>
        </section>
      )}
      <ResearchModelTimeValidation
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={busy}
        recipe={plan}
      />
      <ResearchModelMappings
        kind="model_training"
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={busy}
        model={report}
      />
    </ResearchPage>
  );
}
