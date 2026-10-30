import { useMemo } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import {
  taskOwner,
  timestamp,
  type TaskOwnerStatus,
  type TaskRecord,
  type TaskResult,
} from "../../src/bridge/client";
import { ErrorNotice, getLocale, strategyRule, type TerminalContext } from "../contract";
import { SettlementMarginResults } from "./SettlementMargins";
import { QuantStatsReport } from "./QuantStatsReport";
import { ResearchExport } from "./ResearchExport";
import { BacktestReuse } from "./BacktestReuse";
import { TargetRejectionEvidence } from "./TargetRejectionEvidence";
import { PortfolioBudget } from "./PortfolioBudget";
import { PortfolioTargetsEvidence } from "./PortfolioTargetsEvidence";
import { AccountReconciliation } from "./AccountReconciliation";
import { TradeStatistics } from "./TradeStatistics";
import { MonthlyContributions } from "./MonthlyContributions";
import { ExperimentDetails } from "./ExperimentDetails";
import { Figures, percent, ratio } from "./Performance";
import { namespace, states, t, type Run } from "./shared";

type Evidence = Extract<TaskResult, { kind: "backtest" }>;

// A submitted backtest while it runs: its state, cancel and retry.
export function BacktestProgress({
  activeTask,
  taskService,
  snapshot,
  busy,
  run,
  onResult,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  activeTask: TaskRecord | undefined;
  taskService: TaskOwnerStatus | null | undefined;
  run: Run;
  onResult: (id: string) => void;
}) {
  return (
    <section className="research-progress" aria-label={t("回测进度")}>
      <h3>{activeTask?.instrument || t("等待任务状态")}</h3>
      <p role="status">
        {t(activeTask ? states[activeTask.state] : "等待任务状态")}
        {!taskService?.online && ` · ${t("最后确认状态")}`}
      </p>
      <p>
        {activeTask?.completed ?? 0} / {activeTask?.total ?? 0}
      </p>
      {activeTask && (
        <progress
          value={activeTask.completed}
          max={Math.max(1, activeTask.total)}
          aria-label={t("回测进度")}
        />
      )}
      {activeTask?.error && (
        <p role="alert">
          <ErrorNotice error={activeTask.error} namespace={namespace} />
        </p>
      )}
      <div className="source-actions">
        {activeTask?.state === "succeeded" && (
          <button
            className="primary"
            disabled={busy || !taskService?.online}
            onClick={() => onResult(activeTask.id)}
          >
            {t("查看结果")}
          </button>
        )}
        {activeTask && ["queued", "running"].includes(activeTask.state) && (
          <button
            disabled={busy || !taskService?.online}
            onClick={() =>
              void run("task.action", {
                owner: taskOwner(snapshot?.research_service, "research"),
                id: activeTask.id,
                action: "cancel",
              })
            }
          >
            {t("取消回测")}
          </button>
        )}
        {activeTask && ["failed", "cancelled", "interrupted"].includes(activeTask.state) && (
          <button
            disabled={busy || !taskService?.online}
            onClick={() =>
              void run("task.action", {
                owner: taskOwner(snapshot?.research_service, "research"),
                id: activeTask.id,
                action: "retry",
              })
            }
          >
            {t("重新运行")}
          </button>
        )}
      </div>
      <p className="subtle">{t("任务已提交，可在任务中心查看进度。")}</p>
    </section>
  );
}

// One backtest's fixed parameters and every result view of it.
export function BacktestResult({
  result,
  activeTask,
  taskService,
  snapshot,
  busy,
  query,
  run,
  navigate,
}: Pick<TerminalContext, "snapshot" | "busy" | "query" | "navigate"> & {
  result: Evidence;
  activeTask: TaskRecord | undefined;
  taskService: TaskOwnerStatus | null | undefined;
  run: Run;
}) {
  const equitySeries = useMemo(
    () => ({
      kind: "line" as const,
      points: result.result.equity.map(point => ({
        timestamp_ns: point.timestamp_ns,
        price: point.equity,
      })),
    }),
    [result.result.equity],
  );
  return (
    <section className="research-result" aria-label={t("回测结果")}>
      <h3>
        {activeTask?.instrument} ·{" "}
        {result.experiment.walk_forward
          ? t("滚动验证 · {n} 个候选", { n: result.experiment.strategies.length })
          : strategyRule(result.experiment.strategies[result.result.selected])}
      </h3>
      <p className="subtle">
        {activeTask && new Date(activeTask.submitted_at_ms).toLocaleString(getLocale())} ·{" "}
        {t("本页显示该次任务的固定参数与结果。")}
      </p>
      <div className="research-metrics">
        {[
          [t("初始资金"), result.experiment.paper.deposit],
          [t("期末权益"), result.result.account.equity],
          [t("手续费"), result.result.account.fees],
          [t("最大回撤金额"), result.result.max_drawdown],
          [t("成交笔数"), result.result.account.fills.length],
        ].map(([label, value]) => (
          <div key={label}>
            <span className="subtle">{label}</span>
            <strong>{value}</strong>
          </div>
        ))}
      </div>
      <button
        onClick={() =>
          navigate("workspace.performance", {
            params: { id: result.id, connection_id: taskService!.connection_id },
          })
        }
      >
        {t("查看绩效与归因")}
      </button>
      <Figures label={t("绩效")} value={result.performance} />
      <QuantStatsReport
        key={`quantstats-${taskService?.connection_id}-${result.id}-${result.task.result_digest}`}
        id={result.id}
        resultDigest={result.task.result_digest}
        busy={busy}
        query={query}
      />
      {result.performance.development && (
        <>
          <h4>{t("比较")}</h4>
          <p className="subtle">
            {t(
              "候选策略只按 {day} 之前的交易日比较，取夏普比率最高的一个；其余候选在留出段的表现没有计算，也不显示。",
              { day: result.experiment.holdout_day },
            )}
          </p>
          <div className="research-table">
            <table aria-label={t("候选策略")}>
              <thead>
                <tr>
                  <th>{t("策略")}</th>
                  <th>{t("前段总收益率")}</th>
                  <th>{t("前段夏普比率")}</th>
                  <th>{t("前段最大回撤")}</th>
                </tr>
              </thead>
              <tbody>
                {result.result.candidates.map((candidate, index) => (
                  <tr key={index}>
                    <td>
                      {strategyRule(result.experiment.strategies[index])}
                      {index === result.result.selected ? ` · ${t("已选中")}` : ""}
                    </td>
                    <td>{percent(candidate.total_return)}</td>
                    <td>{ratio(candidate.sharpe)}</td>
                    <td>{percent(candidate.max_drawdown)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <h4>{t("前段")}</h4>
          <Figures label={t("前段")} value={result.performance.development} />
          <h4>{t("留出段")}</h4>
          {result.performance.holdout ? (
            <Figures label={t("留出段")} value={result.performance.holdout} />
          ) : (
            <p className="subtle">{t("前段结束时权益已不为正，留出段没有收益率可言。")}</p>
          )}
        </>
      )}
      {result.experiment.walk_forward && (
        <>
          <h4>{t("滚动验证")}</h4>
          <p className="subtle">
            {t(
              "每一轮只用它的 {training} 个训练交易日给候选策略打分，账户在随后的验证交易日里跟随夏普比率最高的那个；第一轮验证开始之前不持仓。下表的验证段数字是这一个账户在各轮验证交易日里的表现，不是选中策略单独回放的结果。",
              { training: result.experiment.walk_forward.training_days },
            )}
          </p>
          <p className="subtle">
            {t(
              "训练日期是评分区间，不表示策略状态在首日重置；这些边界本身不证明资料未参与此前选择。",
            )}
          </p>
          <div className="research-table">
            <table aria-label={t("滚动验证各轮")}>
              <thead>
                <tr>
                  <th>{t("训练评分区间（含首尾）")}</th>
                  <th>{t("训练交易日数")}</th>
                  <th>{t("验证起始日")}</th>
                  <th>{t("验证末交易日")}</th>
                  <th>{t("跟随的策略")}</th>
                  <th>{t("训练段夏普比率")}</th>
                  <th>{t("验证交易日数")}</th>
                  <th>{t("验证段收益率")}</th>
                  <th>{t("验证段最大回撤")}</th>
                </tr>
              </thead>
              <tbody>
                {result.result.folds.map((fold, index) => {
                  const validation = result.performance.folds[index];
                  return (
                    <tr key={fold.first_day}>
                      <td>
                        {fold.training_first_day} — {fold.training_last_day}
                      </td>
                      <td>{fold.training_days}</td>
                      <td>{fold.first_day}</td>
                      <td>{fold.last_day}</td>
                      <td>
                        {fold.selected === null
                          ? t("无（都没有夏普比率，不持仓）")
                          : strategyRule(result.experiment.strategies[fold.selected])}
                      </td>
                      <td>
                        {ratio(
                          fold.selected === null ? null : fold.candidates[fold.selected].sharpe,
                        )}
                      </td>
                      <td>{fold.validation_days}</td>
                      <td>{percent(validation?.total_return ?? null)}</td>
                      <td>{percent(validation?.max_drawdown ?? null)}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
          <h4>{t("样本外")}</h4>
          {result.performance.out_of_sample ? (
            <Figures label={t("样本外")} value={result.performance.out_of_sample} />
          ) : (
            <p className="subtle">{t("第一轮验证开始时权益已不为正。")}</p>
          )}
        </>
      )}
      <TradeStatistics key={result.id} trades={result.trades} />
      <AccountReconciliation key={`audit-${result.id}`} audit={result.reconciliation} />
      <PortfolioBudget key={`budget-${result.id}`} result={result} query={query} busy={busy} />
      <MonthlyContributions key={`months-${result.id}`} months={result.performance.by_month} />
      <PriceChart
        key={`equity-${taskService?.connection_id}-${result.id}`}
        series={equitySeries}
        label={t("权益曲线")}
        volumeLabel={t("成交量")}
        showVolume={false}
        formatTime={timestamp}
        describe={index =>
          `${timestamp(result.result.equity[index].timestamp_ns)} · ${result.result.equity[index].equity}`
        }
      />
      <details className="research-settlements">
        <summary>{t("逐日结算")}</summary>
        <div className="research-table">
          <table>
            <thead>
              <tr>
                {["交易日", "合约", "结算价", "结算后持仓", "期末权益", "手续费"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {result.result.settlements.flatMap(day =>
                day.contracts.map((contract, index) => (
                  <tr key={`${day.trading_day}.${contract.venue}.${contract.symbol}`}>
                    <td>{index === 0 ? day.trading_day : ""}</td>
                    <td>{contract.symbol}</td>
                    <td>{contract.price}</td>
                    <td>{contract.position_quantity}</td>
                    <td>{index === 0 ? day.equity : ""}</td>
                    <td>{index === 0 ? day.fees : ""}</td>
                  </tr>
                )),
              )}
            </tbody>
          </table>
        </div>
        <p className="subtle">{t("手续费为累计值，权益曲线包含每根 K 线的估值和日终结算。")}</p>
      </details>
      <SettlementMarginResults settlements={result.result.settlements} />
      <PortfolioTargetsEvidence key={`portfolio-${result.id}`} evidence={result} />
      <TargetRejectionEvidence evidence={result} />
      <ExperimentDetails evidence={result} />
      <ResearchExport
        key={`export-${result.id}`}
        id={result.id}
        kind="backtest"
        busy={busy}
        query={query}
      />
      <BacktestReuse
        key={`reuse-${result.id}`}
        evidence={result}
        snapshot={snapshot}
        busy={busy}
        query={query}
        run={run}
      />
      <details>
        <summary>{t("结果详情")}</summary>
        <p>{result.id}</p>
        <p>
          {t("数据版本")} <code>{result.result.dataset_revision}</code>
        </p>
        <p>{t("未平持仓：{count}", { count: result.result.account.positions.length })}</p>
      </details>
    </section>
  );
}
