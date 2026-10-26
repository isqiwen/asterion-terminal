import { useCallback, useEffect, useRef, useState } from "react";
import type {
  ResearchProjectDefinition,
  ResearchProjectDraft,
  ResearchHypothesisPlan,
  ResearchProjectTasks,
} from "../../src/bridge/client";
import { taskOwner } from "../../src/bridge/client";
import {
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { PlanDetails, projectFields } from "./ResearchPlan";
import { ResearchConclusions } from "./ResearchConclusions";
import { ResearchPage, states, t, useRun } from "./shared";
const empty: ResearchProjectDefinition = {
  title: "",
  question: "",
  hypothesis: "",
  mechanism: "",
  scope: "",
  data_needs: "",
  evaluation: "",
  support: "",
  refutation: "",
  limitations: "",
  notes: "",
  sources: [],
};
type Editor = {
  id: string;
  digest: string;
  original: ResearchProjectDefinition;
  definition: ResearchProjectDefinition;
};
function equalDefinition(a: ResearchProjectDefinition, b: ResearchProjectDefinition) {
  return (
    Object.keys(projectFields).every(
      key => a[key as keyof typeof projectFields] === b[key as keyof typeof projectFields],
    ) && JSON.stringify(a.sources) === JSON.stringify(b.sources)
  );
}
const fresh = (): Editor => ({
  id: `project-${crypto.randomUUID()}`,
  digest: "",
  original: empty,
  definition: empty,
});

export function ResearchProjects({ snapshot, query, trade, busy, navigate }: TerminalContext) {
  const service = snapshot?.research_service;
  const ask = useRef(query);
  ask.current = query;
  const destination = JSON.stringify([service?.node_id, service?.service]);
  const [editor, setEditor] = useWorkspaceDraft<Editor>(`project-editor:${destination}`, fresh);
  const [reason, setReason] = useWorkspaceDraft(`project-revision:${destination}`, "");
  const [parent, setParent] = useWorkspaceDraft(`project-parent:${destination}`, "");
  const [preview, setPreview] = useState(false);
  const [drafts, setDrafts] = useState<ResearchProjectDraft[]>([]);
  const [nextDraft, setNextDraft] = useState("");
  const [plans, setPlans] = useState<ResearchHypothesisPlan[]>([]);
  const [nextPlan, setNextPlan] = useState("");
  const [selected, setSelected] = useState<ResearchHypothesisPlan | null>(null);
  const [tasks, setTasks] = useState<ResearchProjectTasks>({ tasks: [], next_before_sequence: 0 });
  const [loading, setLoading] = useState(false);
  const { error, setError, run } = useRun(trade);
  const dirty = !equalDefinition(editor.original, editor.definition);
  useWorkspaceUnsaved(`project-editor:${destination}`, dirty);
  const [saveId, setSaveId] = useWorkspaceRequestId(
    `project-save:${destination}`,
    JSON.stringify([editor.id, editor.digest, editor.definition]),
  );
  const [fixIds, setFixIds] = useWorkspaceDraft<{
    fingerprint: string;
    operation: string;
    plan: string;
  } | null>(`project-fix:${destination}`, null);
  const fingerprint = JSON.stringify([editor.id, editor.digest, parent, reason]);
  const blocked = busy || loading || !service?.online;
  const list = useCallback(async (after = "") => {
    const reply = (await ask.current("research.project.list", { after, limit: 30 }))
      .research_project;
    if (!reply) throw new Error(t("研究项目响应缺失"));
    setDrafts(previous => (after ? [...previous, ...reply.drafts] : reply.drafts));
    setNextDraft(reply.next_after);
  }, []);
  useEffect(() => {
    let active = true;
    void list().catch(error => {
      if (active) setError(asDisplayError(error));
    });
    return () => {
      active = false;
    };
  }, [list, destination, setError]);
  async function perform(action: () => Promise<void>) {
    setLoading(true);
    setError("");
    try {
      await action();
    } catch (error) {
      setError(asDisplayError(error));
    } finally {
      setLoading(false);
    }
  }
  const discard = () => !dirty || window.confirm(t("放弃当前未保存的研究草稿？"));
  async function history(id: string, after = "") {
    const reply = (await query("research.plan.list", { project_id: id, after, limit: 30 }))
      .research_project;
    if (!reply) throw new Error(t("研究项目响应缺失"));
    setPlans(previous => (after ? [...previous, ...reply.plans] : reply.plans));
    setNextPlan(reply.next_after);
  }
  async function taskPage(id: string, before = 0) {
    const page = (
      await query("research.project.tasks", { project_id: id, before_sequence: before, limit: 30 })
    ).project_tasks;
    if (!page) throw new Error(t("研究项目响应缺失"));
    setTasks(previous => ({
      tasks: before ? [...page.tasks, ...previous.tasks] : page.tasks,
      next_before_sequence: page.next_before_sequence,
    }));
  }
  async function open(id: string) {
    if (!discard()) return;
    await perform(async () => {
      const draft = (await query("research.project.read", { id })).research_project?.draft;
      if (!draft) throw new Error(t("研究项目响应缺失"));
      setEditor({
        id: draft.id,
        digest: draft.digest,
        original: draft.definition,
        definition: draft.definition,
      });
      setParent("");
      setReason("");
      setPreview(false);
      setSelected(null);
      await Promise.all([history(id), taskPage(id)]);
    });
  }
  async function save() {
    const operation = saveId ?? `project-save-${crypto.randomUUID()}`;
    setSaveId(operation);
    const submitted = editor;
    if (
      !(await run("research.project.save", {
        id: operation,
        project_id: submitted.id,
        expected_digest: submitted.digest,
        definition: submitted.definition,
      }))
    )
      return;
    await perform(async () => {
      const draft = (await query("research.project.read", { id: submitted.id })).research_project
        ?.draft;
      if (!draft || !equalDefinition(draft.definition, submitted.definition))
        throw new Error(t("当前草稿已由其他操作修改，请保留本地内容并重新核对。"));
      setEditor(previous =>
        previous === submitted
          ? { ...previous, digest: draft.digest, original: draft.definition }
          : previous,
      );
      setSaveId(null);
      await list();
    });
  }
  async function fix() {
    const ids =
      fixIds?.fingerprint === fingerprint
        ? fixIds
        : {
            fingerprint,
            operation: `project-fix-${crypto.randomUUID()}`,
            plan: `plan-${crypto.randomUUID()}`,
          };
    setFixIds(ids);
    if (
      !(await run("research.plan.fix", {
        id: ids.operation,
        project_id: editor.id,
        plan_id: ids.plan,
        expected_draft_digest: editor.digest,
        parent_plan_id: parent,
        revision_reason: reason,
      }))
    )
      return;
    await perform(async () => {
      const plan = (await query("research.plan.read", { id: ids.plan })).research_project?.plan;
      if (!plan) throw new Error(t("研究项目响应缺失"));
      setSelected(plan);
      setFixIds(null);
      setPreview(false);
      await history(editor.id);
    });
  }
  return (
    <ResearchPage taskService={service} error={error}>
      <p>
        {t("研究草稿、固定计划、计算结果与人工结论分别保存。修改标准不会改写旧计划或正式结果。")}
      </p>
      <div className="research-layout">
        <aside className="research-config">
          <button
            disabled={blocked}
            onClick={() => {
              if (discard()) {
                setEditor(fresh());
                setParent("");
                setReason("");
                setSelected(null);
                setPlans([]);
                setTasks({ tasks: [], next_before_sequence: 0 });
                setPreview(false);
              }
            }}
          >
            {t("新建研究项目")}
          </button>
          <button disabled={blocked} onClick={() => void perform(() => list())}>
            {t("刷新项目")}
          </button>
          {drafts.map(draft => (
            <p key={draft.id}>
              <button
                disabled={blocked}
                aria-pressed={editor.id === draft.id}
                onClick={() => void open(draft.id)}
              >
                {draft.definition.title}
              </button>
            </p>
          ))}
          {nextDraft && (
            <button disabled={blocked} onClick={() => void perform(() => list(nextDraft))}>
              {t("更多项目")}
            </button>
          )}
        </aside>
        <main className="research-main">
          <section aria-label={t("研究草稿")}>
            <h3>{t("研究草稿")}</h3>
            <code>{editor.id}</code>
            <p role="status">
              {t(dirty ? "有未保存的草稿" : editor.digest ? "草稿已保存" : "尚未保存")}
            </p>
            {Object.entries(projectFields).map(([key, label]) => (
              <label key={key}>
                {t(label)}
                <textarea
                  aria-label={t(label)}
                  disabled={blocked}
                  value={editor.definition[key as keyof typeof projectFields]}
                  onChange={event => {
                    const value = event.target.value;
                    setPreview(false);
                    setEditor(previous => ({
                      ...previous,
                      definition: { ...previous.definition, [key]: value },
                    }));
                  }}
                />
              </label>
            ))}
            <label>
              {t("参考来源（每行一项）")}
              <textarea
                aria-label={t("参考来源（每行一项）")}
                disabled={blocked}
                value={editor.definition.sources.join("\n")}
                onChange={event => {
                  const sources = event.target.value.split("\n");
                  setEditor(previous => ({
                    ...previous,
                    definition: { ...previous.definition, sources },
                  }));
                }}
              />
            </label>
            <button
              disabled={blocked || !editor.definition.title.trim() || (!dirty && !!editor.digest)}
              onClick={() => void save()}
            >
              {saveId ? t("确认草稿保存") : t("保存研究草稿")}
            </button>
          </section>
          <section aria-label={t("固定研究计划")}>
            <h3>{t("固定研究计划")}</h3>
            <p>{t("请先保存草稿，再核对原文并固定。没有支持或否定标准的计划标记为探索性。")}</p>
            <label>
              {t("前一计划")}
              <select
                aria-label={t("前一计划")}
                value={parent}
                disabled={blocked}
                onChange={event => {
                  setParent(event.target.value);
                  setPreview(false);
                }}
              >
                <option value="">{t("无前一计划")}</option>
                {plans.map(plan => (
                  <option key={plan.id} value={plan.id}>
                    {plan.id}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {t("修订理由")}
              <textarea
                aria-label={t("修订理由")}
                value={reason}
                disabled={blocked}
                onChange={event => {
                  setReason(event.target.value);
                  setPreview(false);
                }}
              />
            </label>
            <label>
              <input
                type="checkbox"
                checked={preview}
                disabled={blocked || dirty || !editor.digest}
                onChange={event => setPreview(event.target.checked)}
              />
              {t("已核对上方原文，固定后保留原标准")}
            </label>
            <button
              disabled={
                blocked ||
                dirty ||
                !editor.digest ||
                !editor.definition.question.trim() ||
                !editor.definition.hypothesis.trim() ||
                (!!parent && !reason.trim()) ||
                !preview
              }
              onClick={() => void fix()}
            >
              {t("固定计划")}
            </button>
            {editor.digest && (
              <button disabled={blocked} onClick={() => void perform(() => history(editor.id))}>
                {t("读取固定计划")}
              </button>
            )}
            {plans.map(plan => (
              <p key={plan.id}>
                <button disabled={blocked} onClick={() => setSelected(plan)}>
                  {plan.id} · {t(plan.exploratory ? "探索性计划" : "已记录支持与否定标准")}
                </button>
              </p>
            ))}
            {nextPlan && (
              <button
                disabled={blocked}
                onClick={() => void perform(() => history(editor.id, nextPlan))}
              >
                {t("更多计划")}
              </button>
            )}
            {selected && (
              <>
                <PlanDetails plan={selected} />
                <ResearchConclusions
                  key={JSON.stringify([selected.owner, selected.id])}
                  plan={selected}
                  snapshot={snapshot}
                  query={query}
                  trade={trade}
                  busy={busy}
                />
                <button
                  disabled={blocked}
                  onClick={() =>
                    navigate("workspace.factor", {
                      params: {
                        project_id: selected.project_id,
                        plan_id: selected.id,
                        definition_digest: selected.definition_digest,
                        plan_node: selected.owner.node_id,
                        plan_service: selected.owner.service_id,
                      },
                    })
                  }
                >
                  {t("按此计划配置因子实验")}
                </button>
                <button
                  disabled={blocked}
                  onClick={() =>
                    navigate("workspace.backtest", {
                      page: "backtest",
                      params: {
                        project_id: selected.project_id,
                        plan_id: selected.id,
                        definition_digest: selected.definition_digest,
                        plan_node: selected.owner.node_id,
                        plan_service: selected.owner.service_id,
                        plan_request: crypto.randomUUID(),
                      },
                    })
                  }
                >
                  {t("按此计划配置回测实验")}
                </button>
              </>
            )}
          </section>
          <section aria-label={t("项目实验记录")}>
            <h3>{t("项目实验记录")}</h3>
            <p>
              {t(
                "仅列任务接纳时已固定此项目计划的任务；失败、取消和中断也保留。独立实验不会被自动追认。",
              )}
            </p>
            <button
              disabled={blocked || !editor.digest}
              onClick={() => void perform(() => taskPage(editor.id))}
            >
              {t("刷新实验记录")}
            </button>
            {[...tasks.tasks].reverse().map(task => (
              <p key={task.id}>
                <code>{task.id}</code> · {t(states[task.state] ?? task.state)} ·{" "}
                <code>{task.research_plan?.plan_id}</code>
                {task.error && <span>{task.error}</span>}
                {task.state === "succeeded" && (
                  <button
                    disabled={blocked}
                    onClick={async () => {
                      if (
                        await run("task.result", {
                          owner: taskOwner(service, "research"),
                          id: task.id,
                        })
                      )
                        navigate(
                          task.kind === "backtest"
                            ? "workspace.backtest"
                            : task.kind === "sample"
                              ? "workspace.research-samples"
                              : task.kind === "statistics"
                                ? "workspace.research-statistics"
                                : "workspace.factor",
                          task.kind === "backtest"
                            ? { page: "backtest", params: { result_id: task.id } }
                            : undefined,
                        );
                    }}
                  >
                    {t("查看结果")}
                  </button>
                )}
                {["queued", "running"].includes(task.state) && (
                  <button
                    disabled={blocked}
                    onClick={async () => {
                      if (
                        await run("task.action", {
                          owner: taskOwner(service, "research"),
                          id: task.id,
                          action: "cancel",
                        })
                      )
                        await perform(() => taskPage(editor.id));
                    }}
                  >
                    {t("取消")}
                  </button>
                )}
              </p>
            ))}
            {tasks.next_before_sequence > 0 && (
              <button
                disabled={blocked}
                onClick={() => void perform(() => taskPage(editor.id, tasks.next_before_sequence))}
              >
                {t("更早的实验")}
              </button>
            )}
          </section>
        </main>
      </div>
    </ResearchPage>
  );
}
