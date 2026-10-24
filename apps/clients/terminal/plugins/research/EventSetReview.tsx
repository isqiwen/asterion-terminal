import { useMemo, useState } from "react";
import { taskOwner, type NoticeObject } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { ResearchPage, TaskRecords, t, useRun } from "./shared";
export function EventSetReview(context: TerminalContext) {
  const service = context.snapshot?.research_service;
  const { error, run } = useRun(context.trade);
  const [id, setId] = useState("");
  const params = context.workspaceParams;
  const selection = useMemo(() => {
    try {
      if (!params?.source || !params.scope) return null;
      const source = JSON.parse(params.source) as NoticeObject;
      if (
        source.domain !== "intelligence" ||
        source.kind !== "intelligence_set" ||
        ![source.node, source.service, source.id, source.version].every(
          v => typeof v === "string" && !!v,
        )
      )
        return null;
      return { source, use_scope: params.scope };
    } catch {
      return null;
    }
  }, [params]);
  const result = context.snapshot?.task_result;
  const evidence =
    result?.kind === "event_set_review" &&
    result.result.owner.node === service?.owner_node_id &&
    result.result.owner.service === service?.service
      ? result
      : null;
  return (
    <ResearchPage title={t("固定事件集合审阅")} taskService={service} error={error}>
      <p>
        {t(
          "审阅原集合的固定成员、截止、用途和排除证据，不重选来源、不计算预测收益或授予交易权限。",
        )}
      </p>
      {selection ? (
        <section aria-label={t("待审阅固定集合")}>
          <code>
            {selection.source.node}/{selection.source.service}/{selection.source.id}/
            {selection.source.version}
          </code>
          <p>
            {t("使用范围")}：{selection.use_scope}
          </p>
          <label>
            {t("集合审阅任务 ID")}
            <input value={id} disabled={context.busy} onChange={e => setId(e.target.value)} />
          </label>
          <button disabled={context.busy} onClick={() => setId(crypto.randomUUID())}>
            {t("新建集合审阅 ID")}
          </button>
          <button
            disabled={context.busy || !service?.online || !id}
            onClick={() =>
              void run("event.set.review.submit", {
                id,
                owner: taskOwner(service, "research"),
                ...selection,
              })
            }
          >
            {t("提交或确认原集合审阅")}
          </button>
        </section>
      ) : (
        <p>{t("请从已固定发布的事件集合交接；已有任务可直接读取结果。")}</p>
      )}
      <TaskRecords
        taskService={service}
        busy={context.busy}
        query={context.query}
        trade={context.trade}
        run={run}
        listed={task => task.kind === "event_set_review"}
        onResult={task => {
          void run("task.result", { id: task.id, owner: taskOwner(service, "research") });
        }}
      />
      {evidence && (
        <section aria-label={t("集合固定审阅结果")}>
          <h3>{evidence.result.name}</h3>
          <p>
            {t("固定截止毫秒")}：{evidence.result.as_of_ms} · {evidence.result.use_mode}
          </p>
          <p>
            {t("实际组装可用时间")}：{evidence.result.assembled_at_ms}
          </p>
          <p>{t("集合组装时间不代表集合在历史截止时已存在。")}</p>
          <p>
            {t("使用范围")}：{evidence.result.use_scope}
          </p>
          <p>
            {t("实际研究程序")}：<code>{evidence.result.program_artifact}</code>
          </p>
          <p>
            {t("输入摘要")}：<code>{evidence.input_sha256}</code>
          </p>
          <p>
            {t("明确选择的事件数")}：{evidence.result.events.length}
          </p>
          <h4>{t("固定成员审阅")}</h4>
          {evidence.result.reports.map((r, i) => (
            <article key={r.source.id}>
              <code>{evidence.result.members[i].event.event.reference.id}</code>
              <p>
                {r.contract} · {r.margin_ratio} · {r.effective_day}
              </p>
              <p>
                {t(r.strict_history_eligible ? "满足严格实际历史资格" : "不满足严格实际历史资格")}
              </p>
              <ul>
                {r.limitations.map(v => (
                  <li key={v}>{v}</li>
                ))}
                {evidence.result.members[i].limitations.map((v, j) => (
                  <li key={j}>
                    {v.reason} · {v.source.id}
                  </li>
                ))}
              </ul>
            </article>
          ))}
          <h4>{t("排除证据")}</h4>
          <ul>
            {evidence.result.exclusions.map(e => (
              <li key={e.event.id}>
                <code>{e.event.id}</code>
                <ul>
                  {e.reasons.map((r, i) => (
                    <li key={i}>
                      {r.reason} · {r.source.id}
                    </li>
                  ))}
                </ul>
              </li>
            ))}
          </ul>
        </section>
      )}
    </ResearchPage>
  );
}
