import { taskOwner, type TaskOwnerRef } from "../../src/bridge/client";
import { EvaluationReports, type EvaluationSource } from "./EvaluationReports";
import { useEffect, useRef, useState } from "react";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import type { BacktestEvidence } from "./BacktestComparison";
import { Figures } from "./Performance";
import { QuantStatsReport } from "./QuantStatsReport";
import { MonthlyContributions } from "./MonthlyContributions";
import { TradeStatistics } from "./TradeStatistics";
import { namespace, t } from "./shared";

export function Evaluation({
  snapshot,
  query,
  trade,
  busy,
  workspaceParams,
  navigate,
  focus = "performance",
}: TerminalContext & { focus?: "performance" | "risk" }) {
  const heading = focus === "risk" ? "风险" : "绩效与归因";
  const connection = snapshot?.research_service?.connection_id;
  const sourceId =
    workspaceParams?.connection_id === connection &&
    workspaceParams?.kind !== "performance_evaluation"
      ? (workspaceParams?.id ?? "")
      : "";
  const destination = JSON.stringify([
    connection,
    snapshot?.research_service?.owner_node_id,
    snapshot?.research_service?.service,
  ]);
  const [id, setId] = useState(sourceId);
  const [readResult, setReadResult] = useState<{
    result: BacktestEvidence;
    owner: TaskOwnerRef;
    businessNode: string;
  } | null>(null);
  const result = readResult?.result ?? null;
  const fixedSource: EvaluationSource | null = readResult
    ? {
        object: {
          node: readResult.businessNode,
          service: readResult.owner.service,
          domain: "research",
          kind: "backtest_result",
          id: readResult.result.id,
          version: readResult.result.task.result_digest,
        },
        owner: readResult.owner,
        program: readResult.result.task.program_artifact ?? "",
        retired: Boolean(readResult.result.task.retired),
        scopes: [
          "full_account",
          ...(readResult.result.experiment.holdout_day
            ? ["development" as const, "holdout" as const]
            : []),
          ...(readResult.result.experiment.walk_forward ? ["out_of_sample" as const] : []),
        ],
      }
    : null;
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  useEffect(() => {
    generation.current++;
    setId(sourceId);
    setReadResult(null);
    setError("");
    setLoading(false);
    return () => {
      // Invalidates pending replies, not a DOM reference.
      // eslint-disable-next-line react-hooks/exhaustive-deps
      generation.current++;
    };
  }, [destination, sourceId]);
  async function read() {
    const current = ++generation.current;
    setLoading(true);
    setReadResult(null);
    setError("");
    try {
      const owner = taskOwner(snapshot?.research_service, "research");
      const businessNode = snapshot?.research_service?.owner_node_id ?? "";
      const reply = await query("task.result.read", { owner, id });
      if (current !== generation.current) return;
      if (reply.task_result?.kind !== "backtest" || reply.task_result.id !== id)
        throw new Error(t("所选任务没有回测结果"));
      setReadResult({ result: reply.task_result, owner, businessNode });
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setLoading(false);
    }
  }
  return (
    <section className="evaluation-workspace" aria-label={t(heading)}>
      <h2>{t(heading)}</h2>
      <p className="subtle">
        {t(
          focus === "risk"
            ? "可固定原日采样损失、末日敞口资金及显式单合约静态压力；尾部估计、动态压力和完整策略归因仍未实现。"
            : "读取固定账户事实并显式保存绩效与所选风险情景；完整归因、尾部、动态压力与独立性仍有缺项，人工解释另行明确记录。",
        )}
      </p>
      <fieldset disabled={busy || loading || !snapshot?.research_service?.online}>
        <label>
          {t("评估来源")}
          <select
            value={id}
            onChange={event => {
              setId(event.target.value);
              setReadResult(null);
              setError("");
            }}
          >
            <option value="">{t("选择已完成回测")}</option>
            {id && !snapshot?.research_service?.tasks.some(task => task.id === id) && (
              <option value={id}>{id}</option>
            )}
            {(snapshot?.research_service?.tasks ?? [])
              .filter(task => task.kind === "backtest" && task.state === "succeeded")
              .map(task => (
                <option key={task.id} value={task.id}>
                  {task.id} · {task.instrument}
                </option>
              ))}
          </select>
        </label>
        <button disabled={!id} onClick={() => void read()}>
          {t("读取评估来源")}
        </button>
      </fieldset>
      <ErrorNotice error={error} namespace={namespace} />
      <EvaluationReports
        snapshot={snapshot}
        query={query}
        trade={trade}
        busy={busy}
        workspaceParams={workspaceParams}
        source={fixedSource}
      />
      {result && (
        <>
          <p>
            {result.id} · <code>{result.task.result_digest}</code>
          </p>
          <button
            onClick={() =>
              navigate("workspace.backtest", {
                page: "task",
                params: { id: result.id, kind: "backtest" },
              })
            }
          >
            {t("查看原始实验")}
          </button>
          <Figures label={t("绩效")} value={result.performance} />
          <p className="subtle">
            {t(
              "按原固定账户事实读取统计，不改写账本或原发布评分。年化收益使用实际观测秒数和365.2425天；波动率和夏普使用明确年期数与无风险假设，缺项分别显示原因。",
            )}
          </p>
          <MonthlyContributions months={result.performance.by_month} />
          <TradeStatistics trades={result.trades} />
          <QuantStatsReport
            key={`${connection}:${result.id}:${result.task.result_digest}`}
            id={result.id}
            resultDigest={result.task.result_digest}
            busy={busy}
            query={query}
          />
        </>
      )}
    </section>
  );
}
