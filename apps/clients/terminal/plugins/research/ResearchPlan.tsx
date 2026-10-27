import { useEffect, useRef, useState } from "react";
import type {
  ResearchHypothesisPlan,
  ResearchPlanBinding,
  ResearchProjectDefinition,
} from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { t } from "./shared";

export const projectFields: Record<keyof Omit<ResearchProjectDefinition, "sources">, string> = {
  title: "项目标题",
  question: "研究问题",
  hypothesis: "可检验假设",
  mechanism: "预期机制",
  scope: "适用范围",
  data_needs: "所需数据",
  evaluation: "评价方法",
  support: "支持标准",
  refutation: "否定标准",
  limitations: "限制与风险",
  notes: "研究笔记",
};
export function planBinding(plan: ResearchHypothesisPlan): ResearchPlanBinding {
  return {
    project_id: plan.project_id,
    plan_id: plan.id,
    definition_digest: plan.definition_digest,
    owner: plan.owner,
  };
}
export function samePlan(a: ResearchPlanBinding, b: ResearchPlanBinding) {
  return (
    a.project_id === b.project_id &&
    a.plan_id === b.plan_id &&
    a.definition_digest === b.definition_digest &&
    a.owner.domain === b.owner.domain &&
    a.owner.node_id === b.owner.node_id &&
    a.owner.service_id === b.owner.service_id
  );
}
export function ProjectDefinition({ definition }: { definition: ResearchProjectDefinition }) {
  return (
    <dl>
      {Object.entries(projectFields).map(([key, label]) => (
        <div key={key}>
          <dt>{t(label)}</dt>
          <dd style={{ whiteSpace: "pre-wrap" }}>
            {definition[key as keyof typeof projectFields] || t("未填写")}
          </dd>
        </div>
      ))}
      <dt>{t("参考来源")}</dt>
      <dd>{definition.sources.join("\n") || t("未填写")}</dd>
    </dl>
  );
}
export function PlanDetails({ plan }: { plan: ResearchHypothesisPlan }) {
  return (
    <section aria-label={t("固定研究计划")}>
      <p>
        <code>{plan.id}</code> · {t(plan.exploratory ? "探索性计划" : "已记录支持与否定标准")}
      </p>
      <p>
        {t("计划版本")} · <code>{plan.definition_digest}</code>
      </p>
      {plan.parent_plan_id && (
        <p>
          {t("前一计划")} · <code>{plan.parent_plan_id}</code> · {plan.revision_reason}
        </p>
      )}
      <ProjectDefinition definition={plan.definition} />
      <p className="subtle">{t("固定计划保留原标准；计算指标不自动代表假设成立。")}</p>
    </section>
  );
}
export function ResearchPlanEvidence({
  binding,
  query,
}: {
  binding?: ResearchPlanBinding;
  query: TerminalContext["query"];
}) {
  const [plan, setPlan] = useState<ResearchHypothesisPlan | null>(null);
  const ask = useRef(query);
  ask.current = query;
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    setPlan(null);
    setError("");
    if (binding)
      void ask
        .current("research.plan.read", { id: binding.plan_id })
        .then(reply => {
          const result = reply.research_project?.plan;
          if (!result || !samePlan(binding, planBinding(result)))
            throw new Error(t("原计划身份或版本不匹配"));
          if (active) setPlan(result);
        })
        .catch(reason => {
          if (active) setError(asDisplayError(reason));
        });
    return () => {
      active = false;
    };
  }, [binding]);
  if (!binding) return <p className="subtle">{t("此任务未固定研究计划，不代表事前假设检验。")}</p>;
  return (
    <details>
      <summary>{t("任务原研究计划")}</summary>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {plan ? <PlanDetails plan={plan} /> : !error && <p>{t("正在读取原计划")}</p>}
    </details>
  );
}

export function ResearchPlanPicker({
  snapshot,
  query,
  binding,
  onChange,
  onReady,
}: Pick<TerminalContext, "snapshot" | "query"> & {
  binding: ResearchPlanBinding | null;
  onChange: (binding: ResearchPlanBinding | null) => void;
  onReady: (ready: boolean) => void;
}) {
  const [plans, setPlans] = useState<ResearchHypothesisPlan[]>([]);
  const ask = useRef(query);
  ask.current = query;
  const [after, setAfter] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const service = snapshot?.research_service;
  useEffect(() => {
    let active = true;
    onReady(!binding);
    setError("");
    if (binding) {
      // Native checks the connected service Owner; node_id here is only a
      // Manager connection alias. Compare the returned stable plan identity.
      void ask
        .current("research.plan.read", { id: binding.plan_id })
        .then(reply => {
          const plan = reply.research_project?.plan;
          if (!plan || !samePlan(binding, planBinding(plan)))
            throw new Error(t("原计划身份或版本不匹配"));
          if (active) onReady(true);
        })
        .catch(error => {
          if (active) setError(asDisplayError(error));
        });
    }
    return () => {
      active = false;
    };
  }, [binding, service?.connection_id, onReady]);
  async function list(cursor = "") {
    setLoading(true);
    setError("");
    try {
      const reply = (
        await query("research.plan.list", { project_id: "", after: cursor, limit: 30 })
      ).research_project;
      if (!reply) throw new Error(t("研究项目响应缺失"));
      setPlans(previous => (cursor ? [...previous, ...reply.plans] : reply.plans));
      setAfter(reply.next_after);
    } catch (error) {
      setError(asDisplayError(error));
    } finally {
      setLoading(false);
    }
  }
  return (
    <section aria-label={t("实验研究计划")}>
      <h3>{t("实验研究计划")}</h3>
      <p>{t("可明确选择固定计划，或进行未绑定的探索实验。提交后不能改写原任务计划。")}</p>
      <button type="button" disabled={loading || !service?.online} onClick={() => void list()}>
        {t("读取固定计划")}
      </button>
      <select
        aria-label={t("选择固定计划")}
        value={binding?.plan_id ?? ""}
        disabled={loading}
        onChange={event => {
          const plan = plans.find(plan => plan.id === event.target.value);
          onReady(!event.target.value);
          if (!event.target.value) onChange(null);
          else if (plan) onChange(planBinding(plan));
        }}
      >
        <option value="">{t("独立探索（不绑定计划）")}</option>
        {binding && !plans.some(plan => plan.id === binding.plan_id) && (
          <option value={binding.plan_id}>{binding.plan_id}</option>
        )}
        {plans.map(plan => (
          <option key={plan.id} value={plan.id}>
            {plan.definition.title} · {plan.id}
          </option>
        ))}
      </select>
      {after && (
        <button type="button" disabled={loading} onClick={() => void list(after)}>
          {t("更多计划")}
        </button>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {binding && <ResearchPlanEvidence binding={binding} query={query} />}
    </section>
  );
}

// The research plan a backtest records, and a plan handed over in the
// workspace parameters, taken once; `opened` is told when one arrives.
export function usePlanBinding(
  connection: string | undefined,
  workspaceParams: TerminalContext["workspaceParams"],
  onOpened: () => void,
) {
  const opened = useRef(onOpened);
  opened.current = onOpened;
  const [plan, setPlan] = useWorkspaceDraft<ResearchPlanBinding | null>(
    `backtest-plan:${connection}`,
    null,
  );
  const [planReady, setPlanReady] = useState(false);
  const planHint = workspaceParams?.plan_id ? JSON.stringify(workspaceParams) : "";
  const [seenPlanHint, setSeenPlanHint] = useWorkspaceDraft(`backtest-plan-hint:${connection}`, "");
  useEffect(() => {
    if (!planHint || planHint === seenPlanHint) return;
    const params = JSON.parse(planHint) as Record<string, string>;
    setPlanReady(false);
    setPlan({
      project_id: params.project_id,
      plan_id: params.plan_id,
      definition_digest: params.definition_digest,
      owner: { domain: "research", node_id: params.plan_node, service_id: params.plan_service },
    });
    setSeenPlanHint(planHint);
    opened.current();
  }, [planHint, seenPlanHint, setSeenPlanHint, setPlan]);
  const researchReady = planReady && (!planHint || planHint === seenPlanHint);
  return { plan, setPlan, setPlanReady, researchReady };
}
