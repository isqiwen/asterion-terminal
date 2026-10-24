import { useMemo } from "react";
import { taskOwner, type NoticeObject } from "../../src/bridge/client";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { ResearchPage, TaskRecords, t, useRun } from "./shared";
import { NoticeSourceChanges } from "./NoticeSourceChanges";
const limitations: Record<string, string> = {
  fixed_announcement_evidence_only: "仅审阅固定公告证据",
  no_prediction_validation: "未验证预测有效性",
  source_publication_time_unknown: "来源发布时间未知",
  source_publication_time_inconsistent: "来源发布时间不一致",
  post_hoc_publication: "发布仅声明事后研究用途",
  publication_after_cutoff: "情报实际可用时间晚于审阅截止时点",
};
const utc = (value: string | null) => {
  if (!value) return t("未知");
  const date = new Date(Number(value));
  return Number.isFinite(date.getTime()) ? date.toISOString() : value;
};
export function NoticeEvidenceReview(context: TerminalContext) {
  const { snapshot, busy, query, trade, workspaceParams } = context;
  const service = snapshot?.research_service;
  const { error, run } = useRun(trade);
  const [id, setId] = useWorkspaceDraft("notice-review-id", "");
  const [custodyId, setCustodyId] = useWorkspaceDraft("notice-custody-id", "");
  const [cutoff, setCutoff] = useWorkspaceDraft(
    "notice-review-cutoff",
    new Date().toISOString().slice(0, 23),
  );
  const selection = useMemo(() => {
    try {
      if (!workspaceParams?.source || !workspaceParams.scope) return null;
      const source = JSON.parse(workspaceParams.source) as NoticeObject;
      if (
        !source ||
        source.domain !== "intelligence" ||
        source.kind !== "intelligence_publication" ||
        ![source.node, source.service, source.id, source.version].every(
          v => typeof v === "string" && !!v,
        )
      )
        return null;
      return { source, use_scope: workspaceParams.scope };
    } catch {
      return null;
    }
  }, [workspaceParams]);
  const milliseconds = Date.parse(cutoff + "Z");
  const validCutoff = Number.isSafeInteger(milliseconds) && milliseconds > 0;
  const evidence =
    snapshot?.task_result?.kind === "notice_review" &&
    snapshot.task_result.result.owner.node === service?.owner_node_id &&
    snapshot.task_result.result.owner.service === service?.service
      ? snapshot.task_result
      : null;
  const observed = snapshot?.notice_custody;
  const custody =
    observed?.consumer.node === service?.owner_node_id &&
    observed?.consumer.service === service?.service &&
    observed?.consumer.id === custodyId
      ? observed
      : null;
  const custodyStates = {
    prepared: "已持久保留，关系确认待核实",
    confirmed: "源引用关系已确认",
    released: "原消费者已持久解除，源内容并未删除",
  };
  return (
    <ResearchPage title={t("公告证据与时点审阅")} taskService={service} error={error}>
      <p>
        {t(
          "固定情报发布版本、截止时点和实际研究程序，只审阅证据与可用性，不计算策略收益、预测效果或交易授权。",
        )}
      </p>
      {selection ? (
        <section aria-label={t("待审阅固定公告")}>
          <code>
            {selection.source.node}/{selection.source.service}/{selection.source.id}/
            {selection.source.version}
          </code>
          <p>
            {t("使用范围")}：{selection.use_scope}
          </p>
          <label>
            {t("公告审阅任务 ID")}
            <input value={id} disabled={busy} onChange={e => setId(e.target.value)} />
          </label>
          <button disabled={busy} onClick={() => setId(crypto.randomUUID())}>
            {t("新建审阅 ID")}
          </button>
          <label>
            {t("审阅截止时间（UTC）")}
            <input
              type="datetime-local"
              step="0.001"
              value={cutoff}
              disabled={busy}
              onChange={e => setCutoff(e.target.value)}
            />
          </label>
          <button
            disabled={busy || !service?.online || !id || !validCutoff}
            onClick={() =>
              void run("notice.review.submit", {
                id,
                owner: taskOwner(service, "research"),
                source: selection.source,
                as_of_ms: String(milliseconds),
                use_scope: selection.use_scope,
              })
            }
          >
            {t("提交或确认原公告审阅")}
          </button>
        </section>
      ) : (
        <p>{t("请从情报已发布公告进入新审阅；已有任务可直接查看结果。")}</p>
      )}
      <TaskRecords
        taskService={service}
        busy={busy}
        trade={trade}
        query={query}
        run={run}
        listed={task => task.kind === "notice_review"}
        onResult={task => {
          setCustodyId(task.id);
          void run("task.result", { id: task.id, owner: taskOwner(service, "research") });
        }}
      />
      <section aria-label={t("源引用保留核对")}>
        <h3>{t("源引用保留核对")}</h3>
        <p>{t("按所属研究任务重核并续办源引用，不接纳新任务、不下载材料或重新计算。")}</p>
        <label>
          {t("需要重核的任务 ID")}
          <input value={custodyId} disabled={busy} onChange={e => setCustodyId(e.target.value)} />
        </label>
        <button
          disabled={busy || !service?.online || !custodyId}
          onClick={() =>
            void run("notice.custody.reconcile", {
              id: custodyId,
              owner: taskOwner(service, "research"),
            })
          }
        >
          {t("重核并续办源引用")}
        </button>
        {custody && (
          <div role="status">
            <p>{t(custodyStates[custody.phase])}</p>
            <p>{t("以下为最近收到的源回执，不是持续在线证明。")}</p>
            <p>
              {t("源持久更新时间")}：{utc(custody.updated_at_ms)}
            </p>
            <p>
              {t("固定发布")}：<code>{custody.source.version}</code>
            </p>
            <p>
              {t("固定输入")}：<code>{custody.consumer.version}</code>
            </p>
          </div>
        )}
      </section>
      {evidence && (
        <section aria-label={t("公告证据审阅结果")}>
          <h3>
            {evidence.id} · {evidence.result.contract}
          </h3>
          <p>
            {t(evidence.result.published_by_cutoff ? "截至所选时点已可用" : "截至所选时点不可用")}
          </p>
          <p>
            {t(
              evidence.result.strict_history_eligible
                ? "已记录时点满足严格历史使用条件"
                : "不满足严格历史使用条件",
            )}
          </p>
          <p>
            {t("审阅截止时间（UTC）")}：{utc(evidence.result.as_of_ms)}
          </p>
          <p>
            {t("计划生效日")}：{evidence.result.effective_day} · {t("保证金比例")}：
            {evidence.result.margin_ratio}
          </p>
          <dl>
            {(
              [
                ["来源发布时间", evidence.result.source_published_at_ms],
                ["材料取得时间", evidence.result.acquired_at_ms],
                ["情报处理时间", evidence.result.processed_at_ms],
                ["人工核验时间", evidence.result.verified_at_ms],
                ["Data 保存时间", evidence.result.stored_at_ms],
                ["情报可用时间", evidence.result.available_at_ms],
              ] as const
            ).map(([label, value]) => (
              <div key={label}>
                <dt>{t(label)}</dt>
                <dd>{utc(value)}</dd>
              </div>
            ))}
          </dl>
          <p>
            {t("固定输入")}：<code>{evidence.input_sha256}</code>
          </p>
          <p>
            {t("固定发布")}：<code>{evidence.result.source.version}</code>
          </p>
          <p>
            {t("固定内容")}：<code>{evidence.result.content.version}</code>
          </p>
          <p>
            {t("固定执行程序")}：<code>{evidence.result.program_artifact}</code>
          </p>
          <NoticeSourceChanges
            context={context}
            input={{
              id: evidence.id,
              input_sha256: evidence.input_sha256,
              source: evidence.result.source,
              owner: evidence.result.owner,
            }}
          />
          <ul>
            {evidence.result.limitations.map(reason => (
              <li key={reason}>{t(limitations[reason] ?? reason)}</li>
            ))}
          </ul>
        </section>
      )}
    </ResearchPage>
  );
}
