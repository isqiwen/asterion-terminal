import { useMemo } from "react";
import { taskOwner, type TradingFacts } from "../../src/bridge/client";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { ResearchPage, TaskRecords, t, useRun } from "./shared";

const reasons: Record<string, string> = {
  complete_broker_history: "缺少完整柜台历史",
  cashflow_history: "缺少出入金历史",
  settlement_evidence: "缺少结算依据",
  valuation_history: "缺少历史估值",
  component_business_times: "缺少分项业务时间",
  funds_currency: "缺少资金币种来源",
  broker_not_ready: "来源当时未就绪",
  known_fills_not_reconciled: "尚未核对已知成交",
  funds_observation_missing: "未取得资金观察，不能当作零余额",
  source_requires_recovery: "来源当时需要恢复",
  unconfirmed_order_intents: "含未确认委托意图",
};
export function AccountReview(context: TerminalContext) {
  const { snapshot, busy, query, trade, workspaceParams } = context;
  const service = snapshot?.research_service;
  const { error, run } = useRun(trade);
  const [id, setId] = useWorkspaceDraft("account-review-id", "");
  const selection = useMemo(() => {
    try {
      if (
        !workspaceParams?.source ||
        !workspaceParams.account ||
        (workspaceParams.environment !== "real" && workspaceParams.environment !== "simulation")
      )
        return null;
      const source = JSON.parse(workspaceParams.source) as TradingFacts["reference"];
      if (
        !source ||
        source.domain !== "trading" ||
        source.kind !== "trading_facts" ||
        ![source.node, source.service, source.id, source.version].every(
          value => typeof value === "string" && value.length > 0,
        )
      )
        return null;
      return {
        source,
        account_id: workspaceParams.account,
        environment: workspaceParams.environment,
      };
    } catch {
      return null;
    }
  }, [workspaceParams]);
  const evidence =
    snapshot?.task_result?.kind === "account_review" &&
    snapshot.task_result.result.owner.node === service?.owner_node_id &&
    snapshot.task_result.result.owner.service === service?.service
      ? snapshot.task_result
      : null;
  return (
    <ResearchPage taskService={service} error={error}>
      <p>{t("审阅固定账户观察的覆盖与缺口，不计算完整账本绩效，不登录或授权交易。")}</p>
      {selection ? (
        <section aria-label={t("待审阅固定事实")}>
          <p>
            {selection.account_id} ·{" "}
            {t(selection.environment === "real" ? "真实资金账户" : "仿真账户")}
          </p>
          <code>
            {selection.source.node}/{selection.source.service}/{selection.source.id}/
            {selection.source.version}
          </code>
          <p>{t("任务固定此来源和版本；多个来源不随机选择，不可用时不会换源。")}</p>
          <label>
            {t("账户审阅任务 ID")}
            <input value={id} disabled={busy} onChange={event => setId(event.target.value)} />
          </label>
          <button disabled={busy} onClick={() => setId(crypto.randomUUID())}>
            {t("新建审阅 ID")}
          </button>
          <button
            disabled={busy || !service?.online || !id}
            onClick={() =>
              void run("account.review.submit", {
                id,
                owner: taskOwner(service, "research"),
                source: selection.source,
                account_id: selection.account_id,
                environment: selection.environment as "real" | "simulation",
              })
            }
          >
            {t("提交或确认原审阅")}
          </button>
        </section>
      ) : (
        <p>{t("请从交易账户已发布的固定事实进入审阅；已有任务可直接查看结果。")}</p>
      )}
      <TaskRecords
        taskService={service}
        busy={busy}
        trade={trade}
        query={query}
        run={run}
        listed={task => task.kind === "account_review"}
        onResult={task =>
          void run("task.result", { id: task.id, owner: taskOwner(service, "research") })
        }
      />
      {evidence && (
        <section aria-label={t("账户观察审阅结果")}>
          <h3>{t("证据不足以计算完整历史绩效")}</h3>
          <p>
            {evidence.id} · {evidence.result.account_id}
          </p>
          <code>{evidence.result.source.version}</code>
          <p>
            {t("固定执行程序")}：<code>{evidence.result.program_artifact}</code>
          </p>
          <p>
            {t("固定输入")}：<code>{evidence.input_sha256}</code>
          </p>
          <dl>
            <dt>{t("观察持仓记录数")}</dt>
            <dd>{evidence.result.position_records}</dd>
            <dt>{t("观察订单记录数")}</dt>
            <dd>{evidence.result.order_records}</dd>
            <dt>{t("观察成交记录数")}</dt>
            <dd>{evidence.result.trade_records}</dd>
            <dt>{t("未确认委托意图数")}</dt>
            <dd>{evidence.result.unconfirmed_orders}</dd>
          </dl>
          <p>{t("以上仅为固定观察中的记录数，不代表完整交易历史或去重后的历史成交总量。")}</p>
          <ul>
            {evidence.result.limitations.map(reason => (
              <li key={reason}>{t(reasons[reason] ?? reason)}</li>
            ))}
          </ul>
        </section>
      )}
    </ResearchPage>
  );
}
