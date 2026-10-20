import { useEffect, useRef, useState } from "react";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import type { BacktestEvidence } from "./BacktestComparison";
import { Figures } from "./Performance";
import { QuantStatsReport } from "./QuantStatsReport";
import { MonthlyContributions } from "./MonthlyContributions";
import { TradeStatistics } from "./TradeStatistics";
import { namespace, t } from "./shared";

export function Evaluation({ snapshot, query, busy, workspaceParams, navigate }: TerminalContext) {
  const connection = snapshot?.task_service?.connection_id;
  const sourceId = workspaceParams?.connection_id === connection ? (workspaceParams?.id ?? "") : "";
  const [id, setId] = useState(sourceId);
  const [result, setResult] = useState<BacktestEvidence | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  useEffect(() => {
    generation.current++;
    setId(sourceId);
    setResult(null);
    setError("");
    setLoading(false);
    return () => {
      // Invalidates pending replies, not a DOM reference.
      // eslint-disable-next-line react-hooks/exhaustive-deps
      generation.current++;
    };
  }, [connection, sourceId]);
  async function read() {
    const current = ++generation.current;
    setLoading(true);
    setResult(null);
    setError("");
    try {
      const reply = await query("task.result.read", { id });
      if (current !== generation.current) return;
      if (reply.task_result?.kind !== "backtest" || reply.task_result.id !== id)
        throw new Error(t("所选任务没有回测结果"));
      setResult(reply.task_result);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setLoading(false);
    }
  }
  return (
    <section className="evaluation-workspace" aria-label={t("绩效与归因")}>
      <h2>{t("绩效与归因")}</h2>
      <p className="subtle">
        {t(
          "当前评估读取已核验回测结果；实盘现金流调整与统一评估参数尚未实现。原始统计口径随结果展示，不作为实盘准入。",
        )}
      </p>
      <fieldset disabled={busy || loading || !snapshot?.task_service?.online}>
        <label>
          {t("评估来源")}
          <select
            value={id}
            onChange={event => {
              setId(event.target.value);
              setResult(null);
              setError("");
            }}
          >
            <option value="">{t("选择已完成回测")}</option>
            {id && !snapshot?.task_service?.tasks.some(task => task.id === id) && (
              <option value={id}>{id}</option>
            )}
            {(snapshot?.task_service?.tasks ?? [])
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
              "按逐日结算权益计算，是统计值而不是账本数值。年化按这些交易日跨越的日历时间折算，无风险利率取 0；不足 20 个交易日时不计算年化指标。",
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
