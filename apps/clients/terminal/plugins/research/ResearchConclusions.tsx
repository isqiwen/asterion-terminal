import { useCallback, useEffect, useRef, useState } from "react";
import type {
  ResearchHypothesisPlan,
  ResearchProjectConclusion,
  ResearchConclusionWrite,
  TaskResult,
} from "../../src/bridge/client";
import { taskOwner } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceUnsaved,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { FactorResults } from "./FactorResults";
import { ExperimentDetails } from "./ExperimentDetails";
import { EvaluationReportView } from "./EvaluationReports";
import { EvaluationExport } from "./EvaluationExport";
import { planBinding, samePlan, PlanDetails } from "./ResearchPlan";
import { t, useRun } from "./shared";
const verdicts = {
  supported: "人工判断：支持",
  refuted: "人工判断：否定",
  insufficient: "人工判断：证据不足",
} as const;
type ResearchEvidence = Extract<
  TaskResult,
  { kind: "factor" | "sample" | "statistics" | "backtest" | "performance_evaluation" }
>;

export function ResearchConclusions({
  plan,
  snapshot,
  query,
  trade,
  busy,
}: Pick<TerminalContext, "snapshot" | "query" | "trade" | "busy"> & {
  plan: ResearchHypothesisPlan;
}) {
  const scope = JSON.stringify([plan.owner, plan.id]);
  const [draft, setDraft] = useWorkspaceDraft(`conclusion:${scope}`, {
    task: "",
    judgment: "",
    reason: "",
    previous: "",
    revision: "",
  });
  const [pending, setPending] = useWorkspaceDraft<{
    fingerprint: string;
    operation: string;
    id: string;
  } | null>(`conclusion-request:${scope}`, null);
  const [evidence, setEvidence] = useState<ResearchEvidence | null>(null);
  const [rows, setRows] = useState<ResearchProjectConclusion[]>([]);
  const [after, setAfter] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const [loading, setLoading] = useState(false);
  const [readError, setReadError] = useState<DisplayError>("");
  const { error, run } = useRun(trade);
  const ask = useRef(query);
  ask.current = query;
  useWorkspaceUnsaved(
    `conclusion:${scope}`,
    !!draft.reason || !!draft.judgment || !!draft.revision,
  );
  const list = useCallback(
    async (cursor = "") => {
      const reply = (
        await ask.current("research.conclusion.list", {
          project_id: plan.project_id,
          after: cursor,
          limit: 30,
        })
      ).research_project;
      if (!reply) throw new Error(t("研究项目响应缺失"));
      setRows(previous => (cursor ? [...previous, ...reply.conclusions] : reply.conclusions));
      setAfter(reply.next_after);
    },
    [plan.project_id],
  );
  useEffect(() => {
    let active = true;
    void list().catch(error => {
      if (active) setReadError(asDisplayError(error));
    });
    return () => {
      active = false;
    };
  }, [list]);
  async function perform(action: () => Promise<void>) {
    setLoading(true);
    setReadError("");
    try {
      await action();
    } catch (error) {
      setReadError(asDisplayError(error));
    } finally {
      setLoading(false);
    }
  }
  const binding = planBinding(plan);
  const original = evidence?.task.research_plan;
  const retrospective = evidence ? !original || !samePlan(original, binding) : false;
  const fingerprint = JSON.stringify([binding, evidence?.id, evidence?.task.result_digest, draft]);
  const blocked = busy || loading || !snapshot?.research_service?.online;
  async function append() {
    if (!evidence || !draft.judgment || !confirmed) return;
    const ids =
      pending?.fingerprint === fingerprint
        ? pending
        : {
            fingerprint,
            operation: `conclusion-write-${crypto.randomUUID()}`,
            id: `conclusion-${crypto.randomUUID()}`,
          };
    setPending(ids);
    const declaration: ResearchConclusionWrite = {
      id: ids.id,
      plan: binding,
      result: {
        node: plan.owner.node_id,
        service: plan.owner.service_id,
        domain: "research",
        kind: `${evidence.kind}_result`,
        id: evidence.id,
        version: evidence.task.result_digest,
      },
      judgment: draft.judgment as ResearchConclusionWrite["judgment"],
      reason: draft.reason,
      previous_id: draft.previous,
      revision_reason: draft.revision,
    };
    if (!(await run("research.conclusion.append", { id: ids.operation, declaration }))) return;
    await perform(async () => {
      const saved = (await query("research.conclusion.read", { id: ids.id })).research_project
        ?.conclusion;
      if (
        !saved ||
        saved.declaration.id !== ids.id ||
        !samePlan(saved.declaration.plan, binding) ||
        saved.declaration.result.version !== declaration.result.version ||
        saved.declaration.reason !== declaration.reason ||
        saved.declaration.judgment !== declaration.judgment
      )
        throw new Error(t("人工结论保存确认不匹配"));
      setPending(null);
      setDraft(previous => ({ ...previous, reason: "", judgment: "", previous: "", revision: "" }));
      setConfirmed(false);
      await list();
    });
  }
  return (
    <section aria-label={t("人工研究结论")}>
      <h3>{t("人工研究结论")}</h3>
      <p>
        {t("判断由研究者明确追加，固定计划、计算结果与旧结论均不被替换。指标不自动证明假设成立。")}
      </p>
      {(error || readError) && (
        <p role="alert">
          <ErrorNotice error={error || readError} />
        </p>
      )}
      <label>
        {t("正式研究任务身份")}
        <input
          aria-label={t("正式研究任务身份")}
          value={draft.task}
          disabled={blocked}
          onChange={event => {
            setDraft(previous => ({ ...previous, task: event.target.value }));
            setEvidence(null);
            setConfirmed(false);
          }}
        />
      </label>
      <button
        disabled={blocked || !draft.task.trim()}
        onClick={() =>
          void perform(async () => {
            setEvidence(null);
            setConfirmed(false);
            const result = (
              await query("task.result.read", {
                owner: taskOwner(snapshot?.research_service, "research"),
                id: draft.task,
              })
            ).task_result;
            if (
              !result ||
              (result.kind !== "factor" &&
                result.kind !== "sample" &&
                result.kind !== "statistics" &&
                result.kind !== "backtest" &&
                result.kind !== "performance_evaluation") ||
              result.id !== draft.task ||
              !result.task.program_artifact
            )
              throw new Error(t("人工结论需要可核验的正式研究结果"));
            setEvidence(result);
          })
        }
      >
        {t("读取待审阅正式结果")}
      </button>
      {evidence && (
        <>
          <p role="status">
            {t(
              retrospective
                ? "回溯关联：此任务接纳时未固定所选计划"
                : "原任务接纳时已固定所选计划；不证明未提前见过样本",
            )}
          </p>
          <details>
            <summary>{t("核对结论引用的原计划和正式结果")}</summary>
            <PlanDetails plan={plan} />
            {evidence.kind === "performance_evaluation" ? (
              <EvaluationReportView report={evidence} />
            ) : evidence.kind === "backtest" ? (
              <>
                <p>
                  {t("此处读取原模拟账本和条件；完整轨迹在回测结果中，指标不自动生成研究判断。")}
                </p>
                <dl>
                  <dt>{t("期末权益")}</dt>
                  <dd>{evidence.result.account.equity}</dd>
                  <dt>{t("手续费")}</dt>
                  <dd>{evidence.result.account.fees}</dd>
                </dl>
                <ExperimentDetails evidence={evidence} />
              </>
            ) : evidence.kind === "factor" ? (
              <FactorResults evidence={evidence} />
            ) : (
              <>
                <p>{evidence.experiment.request.use_scope}</p>
                {(evidence.kind === "sample" ||
                  evidence.result.source.kind === "sample_result") && (
                  <p>{t("事后探索；历史可得时间未知，完整历史覆盖未获证明。")}</p>
                )}
                {evidence.kind === "statistics" && (
                  <p>
                    {t("固定来源")}：
                    <code>
                      {evidence.result.source.id}/{evidence.result.source.version}
                    </code>{" "}
                    · {t("实际有效数")}：{evidence.result.summary.valid}
                  </p>
                )}
                <table aria-label={t("待审阅原行证据")}>
                  <thead>
                    <tr>
                      <th>{t("观察索引")}</th>
                      <th>{t("观察时间")}</th>
                      <th>{t("原始值")}</th>
                      <th>{t("统计角色或原因")}</th>
                    </tr>
                  </thead>
                  <tbody>
                    {evidence.result.rows.slice(0, 50).map(row => (
                      <tr key={row.event_index}>
                        <td>{row.event_index}</td>
                        <td>{row.observed}</td>
                        <td>{row.value ?? "—"}</td>
                        <td>
                          {"role" in row
                            ? t(
                                row.role === "holdout"
                                  ? "留出区间"
                                  : row.role === "development"
                                    ? "开发区间"
                                    : "全样本",
                              )
                            : t(row.reason)}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
                <p>{t("此处预览前50条原行；完整证据在所属样本或统计报告中读取。")}</p>
              </>
            )}
          </details>
          <p>
            {t("正式结果摘要")} · <code>{evidence.task.result_digest}</code>
          </p>
          <p>
            {t("计算程序")} · <code>{evidence.task.program_artifact}</code>
          </p>
          <label>
            {t("明确人工判断")}
            <select
              aria-label={t("明确人工判断")}
              disabled={blocked}
              value={draft.judgment}
              onChange={event => {
                setDraft(previous => ({ ...previous, judgment: event.target.value }));
                setConfirmed(false);
              }}
            >
              <option value="">{t("尚未作出判断")}</option>
              {Object.entries(verdicts).map(([key, label]) => (
                <option key={key} value={key}>
                  {t(label)}
                </option>
              ))}
            </select>
          </label>
          <label>
            {t("人工判断理由")}
            <textarea
              aria-label={t("人工判断理由")}
              disabled={blocked}
              value={draft.reason}
              onChange={event => {
                setDraft(previous => ({ ...previous, reason: event.target.value }));
                setConfirmed(false);
              }}
            />
          </label>
          <label>
            {t("修订前条结论")}
            <select
              aria-label={t("修订前条结论")}
              disabled={blocked}
              value={draft.previous}
              onChange={event => {
                setDraft(previous => ({ ...previous, previous: event.target.value }));
                setConfirmed(false);
              }}
            >
              <option value="">{t("独立追加结论")}</option>
              {rows
                .filter(
                  row =>
                    samePlan(row.declaration.plan, binding) &&
                    row.declaration.result.id === evidence.id &&
                    row.declaration.result.version === evidence.task.result_digest,
                )
                .map(row => (
                  <option key={row.declaration.id} value={row.declaration.id}>
                    {row.declaration.id}
                  </option>
                ))}
            </select>
          </label>
          {draft.previous && (
            <label>
              {t("结论修订理由")}
              <textarea
                aria-label={t("结论修订理由")}
                disabled={blocked}
                value={draft.revision}
                onChange={event => {
                  setDraft(previous => ({ ...previous, revision: event.target.value }));
                  setConfirmed(false);
                }}
              />
            </label>
          )}
          <label>
            <input
              type="checkbox"
              checked={confirmed}
              disabled={blocked}
              onChange={event => setConfirmed(event.target.checked)}
            />
            {t("已核对精确计划和正式证据，明确保存人工判断")}
          </label>
          <button
            disabled={
              blocked ||
              !draft.judgment ||
              !draft.reason.trim() ||
              (!!draft.previous && !draft.revision.trim()) ||
              !confirmed
            }
            onClick={() => void append()}
          >
            {t("追加人工结论")}
          </button>
        </>
      )}
      <button disabled={blocked} onClick={() => void perform(() => list())}>
        {t("刷新人工结论")}
      </button>
      {rows.map(row => (
        <article key={row.declaration.id}>
          <h4>{t(verdicts[row.declaration.judgment])}</h4>
          {evidence?.kind === "performance_evaluation" &&
            row.declaration.result.kind === "performance_evaluation_result" &&
            row.declaration.result.id === evidence.id &&
            row.declaration.result.version === evidence.task.result_digest && (
              <EvaluationExport
                report={evidence}
                owner={taskOwner(snapshot?.research_service, "research")}
                conclusion={row}
                query={query}
                busy={blocked}
              />
            )}
          <code>{row.declaration.id}</code>
          <p>
            {t(row.retrospective ? "回溯关联" : "引用任务接纳时的固定计划")} ·{" "}
            <code>{row.declaration.plan.plan_id}</code>
          </p>
          <p style={{ whiteSpace: "pre-wrap" }}>{row.declaration.reason}</p>
          <p>
            <code>{row.declaration.result.id}</code> · <code>{row.declaration.result.version}</code>
          </p>
          {row.declaration.previous_id && (
            <p>
              {t("修订前条结论")} · <code>{row.declaration.previous_id}</code> ·{" "}
              {row.declaration.revision_reason}
            </p>
          )}
        </article>
      ))}
      {after && (
        <button disabled={blocked} onClick={() => void perform(() => list(after))}>
          {t("更多人工结论")}
        </button>
      )}
    </section>
  );
}
