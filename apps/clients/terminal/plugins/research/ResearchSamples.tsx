import { ResearchPlanPicker, ResearchPlanEvidence } from "./ResearchPlan";
import type { ResearchPlanBinding } from "../../src/bridge/client";
import { useState } from "react";
import { taskOwner, type ResearchSampleInput } from "../../src/bridge/client";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type TerminalContext,
} from "../contract";
import { ResearchPage, TaskRecords, t, useRun } from "./shared";
type Fixed = {
  selection: ResearchSampleInput["request"]["selection"];
  owner: ResearchSampleInput["data_owner"];
  count: number;
  revision: string;
};
type Draft = {
  plan: ResearchPlanBinding | null;
  fixed: Fixed | null;
  purpose: string;
  holdout: boolean;
  split: string;
  acknowledged: boolean;
};
export function ResearchSamples({ snapshot, query, trade, busy }: TerminalContext) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([service?.node_id, service?.service]);
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`sample-draft:${destination}`, {
    plan: null,
    fixed: null,
    purpose: "",
    holdout: false,
    split: "",
    acknowledged: false,
  });
  const [operation, setOperation] = useWorkspaceRequestId(
    `sample-submit:${destination}`,
    JSON.stringify(draft),
  );
  const [loading, setLoading] = useState(false);
  const [planReady, setPlanReady] = useState(false);
  const [page, setPage] = useState(0);
  const { error, run } = useRun(trade);
  useWorkspaceUnsaved(`sample-draft:${destination}`, Boolean(draft.fixed && !operation));
  const blocked = busy || loading || !service?.online;
  const locked = blocked || operation !== null;
  const selected =
    snapshot?.datasets.length === 1 && !snapshot.dataset_series.length
      ? snapshot.datasets[0]
      : null;
  const data = snapshot?.data;
  const valid =
    planReady &&
    draft.fixed &&
    draft.purpose.trim() &&
    draft.acknowledged &&
    (!draft.holdout ||
      (/^\d+$/.test(draft.split) &&
        Number.isSafeInteger(Number(draft.split)) &&
        Number(draft.split) > 0 &&
        Number(draft.split) < draft.fixed.count));
  function freeze() {
    if (!selected || !data?.online || !data.owner_node_id) return;
    setDraft({
      ...draft,
      fixed: {
        selection: {
          source_dataset_ids: [...selected.source_dataset_ids],
          daily: { ...selected.daily },
          begin_day: selected.begin_day,
          end_day: selected.end_day,
          contract: `${selected.contract.venue}/${selected.contract.product}/${selected.contract.delivery_month}`,
        },
        owner: { node_id: data.owner_node_id, service_id: data.service },
        count: selected.count,
        revision: selected.revision,
      },
    });
  }
  async function submit() {
    if (!valid || !draft.fixed || !service) return;
    const id = operation ?? `sample-${crypto.randomUUID()}`;
    setOperation(id);
    setLoading(true);
    try {
      await run("research.sample.submit", {
        id,
        owner: taskOwner(service, "research"),
        ...(draft.plan ? { research_plan: draft.plan } : {}),
        data_owner: draft.fixed.owner,
        selection: draft.fixed.selection,
        use_scope: draft.purpose,
        mode: "post_hoc",
        evaluation: draft.holdout
          ? { mode: "holdout", split_index: Number(draft.split) }
          : { mode: "full_sample" },
      });
    } finally {
      setLoading(false);
    }
  }
  const report =
    snapshot?.task_result?.kind === "sample" &&
    snapshot.task_result.result.owner.node_id === service?.owner_node_id &&
    snapshot.task_result.result.owner.service_id === service?.service
      ? snapshot.task_result
      : null;
  const last = Math.max(0, Math.ceil((report?.result.rows.length ?? 0) / 50) - 1);
  const current = Math.min(page, last);
  return (
    <ResearchPage title={t("独立研究样本")} taskService={service} error={error}>
      <p>
        {t("固定单合约标准数据版本，无需先生成因子；保留原始收盘价和实际观察，不补齐缺失行情。")}
      </p>
      <fieldset disabled={locked}>
        <ResearchPlanPicker
          snapshot={snapshot}
          query={query}
          binding={draft.plan}
          onChange={plan => setDraft({ ...draft, plan })}
          onReady={setPlanReady}
        />
        <button disabled={!selected || !data?.online || !data.owner_node_id} onClick={freeze}>
          {t("固定当前数据选择")}
        </button>
        {draft.fixed && (
          <section aria-label={t("固定样本来源")}>
            <code>
              {draft.fixed.owner.node_id}/{draft.fixed.owner.service_id}
            </code>
            <p>
              {draft.fixed.selection.contract} · {draft.fixed.selection.begin_day} —{" "}
              {draft.fixed.selection.end_day}
            </p>
            <code>{draft.fixed.revision}</code>
            <p>
              {t("实际观察数")}：{draft.fixed.count}
            </p>
          </section>
        )}
        <label>
          {t("样本用途")}
          <input
            value={draft.purpose}
            onChange={e => setDraft({ ...draft, purpose: e.target.value })}
          />
        </label>
        <label>
          <input
            type="checkbox"
            checked={draft.holdout}
            onChange={e => setDraft({ ...draft, holdout: e.target.checked })}
          />
          {t("划分留出区间")}
        </label>
        {draft.holdout && (
          <label>
            {t("首个留出观察索引")}
            <input
              type="number"
              min="1"
              value={draft.split}
              onChange={e => setDraft({ ...draft, split: e.target.value })}
            />
          </label>
        )}
        <label>
          <input
            type="checkbox"
            checked={draft.acknowledged}
            onChange={e => setDraft({ ...draft, acknowledged: e.target.checked })}
          />
          {t("确认仅作事后探索：历史可得时间未知，完整历史覆盖未获证明。")}
        </label>
      </fieldset>
      <button disabled={blocked || !valid} onClick={() => void submit()}>
        {t("提交或确认原样本任务")}
      </button>
      {operation && (
        <p>
          {t("样本任务身份")}：<code>{operation}</code>
          <button disabled={blocked} onClick={() => setOperation(null)}>
            {t("新建样本请求")}
          </button>
        </p>
      )}
      <TaskRecords
        taskService={service}
        busy={busy}
        query={query}
        trade={trade}
        run={run}
        listed={task => task.kind === "sample"}
        onResult={task => {
          setPage(0);
          void run("task.result", { id: task.id, owner: taskOwner(service!, "research") });
        }}
      />
      {report && (
        <section aria-label={t("正式样本证据")}>
          <h3>{t("正式样本证据")}</h3>
          <ResearchPlanEvidence binding={report.task.research_plan} query={query} />
          <code>{report.id}</code>
          <p>
            {t("样本用途")}：{report.experiment.request.use_scope}
          </p>
          <p>{t("事后探索；历史可得时间未知，完整历史覆盖未获证明。")}</p>
          <p>
            {t("固定执行程序")}：<code>{report.task.program_artifact}</code>
          </p>
          <code>{report.result.dataset_revision}</code>
          <table>
            <thead>
              <tr>
                {["观察索引", "观察时间", "交易日", "统计角色", "原始收盘价"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {report.result.rows.slice(current * 50, (current + 1) * 50).map(row => (
                <tr key={row.event_index}>
                  <td>{row.event_index}</td>
                  <td>{row.observed}</td>
                  <td>{row.trading_day}</td>
                  <td>
                    {t(
                      row.role === "holdout"
                        ? "留出区间"
                        : row.role === "development"
                          ? "开发区间"
                          : "全样本",
                    )}
                  </td>
                  <td>{row.value ?? "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={current === 0} onClick={() => setPage(current - 1)}>
            {t("上一页")}
          </button>
          <button disabled={current === last} onClick={() => setPage(current + 1)}>
            {t("下一页")}
          </button>
        </section>
      )}
    </ResearchPage>
  );
}
