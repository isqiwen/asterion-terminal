import { useEffect } from "react";
import { taskOwner } from "../../src/bridge/client";
import type { HistoricalMargins, SettlementMargin } from "../../src/bridge/client";
import { FlowSteps } from "../../src/ui/FlowSteps";
import {
  asDisplayError,
  contractCostRequest,
  ContractCosts,
  DatasetPicker,
  StrategyFields,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  type ContractCostDrafts,
  type TerminalContext,
} from "../contract";
import { useBacktestCandidates } from "./BacktestCandidates";
import { BacktestProgress, BacktestResult } from "./BacktestResult";
import {
  AccountLimits,
  BacktestSummary,
  CandidateComparison,
  HistoricalMarginChoice,
  MatchingNotes,
  PerformanceAssumptionFields,
  performanceOf,
} from "./BacktestSettings";
import { PortfolioPolicyEditor } from "./PortfolioPolicy";
import { ResearchPage, selectedSeries, t, TaskRecords, useRun } from "./shared";
import { DeliveryNotice, useStrategyDelivery } from "./StrategyDelivery";

export function Backtest({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  workspacePage,
  workspaceParams,
}: TerminalContext) {
  const taskService = snapshot?.research_service;
  const [view, setView] = useWorkspaceDraft<"records" | "configure" | "progress" | "result">(
    `backtest-view:${taskService?.connection_id}`,
    "records",
  );
  const [step, setStep] = useWorkspaceDraft("backtest-step", 0);
  const [received, setReceived] = useWorkspaceDraft("backtest-source-request", "");
  const [confirmedSource, setConfirmedSource] = useWorkspaceDraft("backtest-source-confirmed", "");
  const sourceRequestKey =
    workspacePage === "backtest" && workspaceParams?.selection_id
      ? JSON.stringify([workspaceParams.selection_id, taskService?.connection_id])
      : "";
  const [selectedTask, setSelectedTask] = useWorkspaceDraft(
    `backtest-task:${taskService?.connection_id}`,
    "",
  );
  const activeTask = taskService?.tasks.find(task => task.id === selectedTask);
  const { error, setError, run } = useRun(trade);
  async function showResult(id: string) {
    if (
      await run("task.result", { owner: taskOwner(snapshot?.research_service, "research"), id })
    ) {
      setSelectedTask(id);
      setView("result");
    }
  }
  const datasets = snapshot?.datasets ?? [];
  const requestedDelivery =
    workspacePage === "backtest" ? workspaceParams?.strategy_delivery_id : "";
  const { binding, boundDataMatches } = useStrategyDelivery({
    query,
    snapshot,
    requestedDelivery,
    connection: taskService?.connection_id,
    opened: task => {
      setSelectedTask(task);
      setView("configure");
      setStep(0);
    },
    failed: setError,
  });

  const dataReady =
    datasets.length > 0 &&
    boundDataMatches &&
    (!sourceRequestKey || confirmedSource === sourceRequestKey);
  useEffect(() => {
    if (sourceRequestKey && received !== sourceRequestKey) {
      setReceived(sourceRequestKey);
      setView("configure");
      setStep(0);
    }
  }, [sourceRequestKey, received, setReceived, setView, setStep]);
  useEffect(() => {
    if (workspacePage !== "task" || !workspaceParams?.id) return;
    setSelectedTask(workspaceParams.id);
    setView("progress");
  }, [workspacePage, workspaceParams, setSelectedTask, setView]);

  useEffect(() => {
    const id = workspaceParams?.result_id;
    if (
      workspacePage === "backtest" &&
      id &&
      snapshot?.task_result?.kind === "backtest" &&
      snapshot.task_result.id === id
    ) {
      setSelectedTask(id);
      setView("result");
    }
  }, [
    workspacePage,
    workspaceParams?.result_id,
    snapshot?.task_result?.kind,
    snapshot?.task_result?.id,
    setSelectedTask,
    setView,
  ]);

  const destination = JSON.stringify([snapshot?.data?.connection_id, taskService?.connection_id]);
  const {
    strategy,
    setStrategy,
    portfolioDraft,
    setPortfolioDraft,
    eligiblePortfolio,
    lockedPortfolio,
    portfolioMode,
    fixedPortfolio,
    portfolioEditorDraft,
    portfolioReady,
    shownStrategy,
    candidates,
  } = useBacktestCandidates(snapshot, binding?.package.definition ?? null);
  // Several values in a window make several strategies, compared before a holdout.
  const [holdoutFrom, setHoldoutFrom] = useWorkspaceDraft("backtest-holdout", "");
  // Or they are compared by rolling: rounds of training days and the
  // validation days after them.
  const [comparison, setComparison] = useWorkspaceDraft<"holdout" | "rolling">(
    "backtest-comparison",
    "holdout",
  );
  const [rolling, setRolling] = useWorkspaceDraft("backtest-rolling", {
    training: "60",
    validation: "20",
  });
  const [performanceDraft, setPerformanceDraft] = useWorkspaceDraft(
    `backtest-performance:${JSON.stringify([destination, datasets.map(item => item.revision)])}`,
    { annual: "", zeroRiskFree: false },
  );
  const { annualPeriods, validAnnualPeriods, performanceReady, performanceAssumptions } =
    performanceOf(performanceDraft, candidates.length);
  // A rule that ranks contracts holds some long and as many short: it needs
  // twice as many as the most any candidate holds a side.
  const selection = selectedSeries(snapshot);
  const ranked = selection.count;
  const held = Math.max(0, ...candidates.map(item => ("count" in item.rule ? item.rule.count : 0)));
  // Ranking by the term structure compares a dominant month with a later one:
  // every unit must be a dominant series with days that allow it.
  const unranked = !candidates.some(item => item.rule.kind === "cross_term_structure")
    ? null
    : selection.alone.length
      ? t("期限结构只在主力连续上评价：请去掉单个合约，只保留主力连续。")
      : selection.dominant.some(series => !series.terms)
        ? t(
            "所选主力连续里有的没有期限结构数据：主力月份之后还需要有已下载日线的月份，才能比较近远月。",
          )
        : null;
  const [parameters, setParameters] = useWorkspaceDraft("backtest-account", {
    deposit: "",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [costs, setCosts] = useWorkspaceDraft<ContractCostDrafts>("contract-costs", {});
  const [settlementMargins, setSettlementMargins] = useWorkspaceDraft<SettlementMargin[]>(
    "backtest-settlement-margins",
    [],
  );
  const supportsSettlementMargins =
    datasets.length === 1 &&
    datasets[0].interval_minutes === 0 &&
    !selection.dominant.length &&
    candidates.length === 1;
  const [marginChoice, setMarginChoice] = useWorkspaceDraft<{
    selection: string;
    value: HistoricalMargins;
  } | null>("backtest-historical-margins", null);
  const marginSelection = JSON.stringify([destination, datasets.map(item => item.revision)]);
  const historicalMargins = marginChoice?.value;
  const invalidSettlementScope =
    ((settlementMargins.length > 0 || !!marginChoice) && !supportsSettlementMargins) ||
    (!!marginChoice &&
      (marginChoice.selection !== marginSelection || settlementMargins.length > 0));
  async function previewMargins() {
    setError("");
    try {
      const reply = await query("backtest.margins.preview", {});
      if (!reply.historical_margins) throw new Error("Historical margin preview is missing");
      setMarginChoice({ selection: marginSelection, value: reply.historical_margins });
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  const [submitted, setSubmitted] = useWorkspaceDraft(`submitted:${destination}:backtest`, "");
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    "backtest-submission",
    JSON.stringify([
      destination,
      strategy,
      portfolioDraft,
      holdoutFrom,
      comparison,
      rolling,
      parameters,
      performanceDraft,
      costs,
      settlementMargins,
      marginChoice,
      datasets.map(item => item.revision),
      binding?.delivery.object.version,
    ]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (!dataReady) {
      setStep(0);
      return;
    }
    if (!performanceReady || !portfolioReady) {
      setStep(1);
      return;
    }
    if (step === 1) {
      setStep(2);
      return;
    }
    const id = binding?.delivery.target_task_id ?? pendingId ?? `backtest-${crypto.randomUUID()}`;
    setPendingId(id);
    try {
      const payload = {
        id,
        ...parameters,
        strategies: candidates,
        ...(performanceAssumptions ? { performance_assumptions: performanceAssumptions } : {}),
        settlement_margins: settlementMargins,
        ...(historicalMargins ? { margin_snapshot: historicalMargins.snapshot } : {}),
        holdout_from: candidates.length > 1 && comparison === "holdout" ? holdoutFrom : "",
        walk_forward:
          candidates.length > 1 && comparison === "rolling"
            ? {
                training_days: Number(rolling.training),
                validation_days: Number(rolling.validation),
              }
            : null,
        contracts: contractCostRequest(datasets, costs, snapshot?.dataset_series ?? []),
        ...(binding
          ? { strategy_use: { package: binding.package.object, delivery: binding.delivery.object } }
          : {}),
      };
      if (await run("backtest.submit", payload)) {
        setSubmitted(id);
        setPendingId(null);
        setSelectedTask(id);
        setView("progress");
      }
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  const result = snapshot?.task_result?.kind === "backtest" ? snapshot.task_result : null;
  return (
    <ResearchPage taskService={taskService} error={error}>
      <div className="workflow-heading research-navigation">
        <h3>
          {t(
            view === "records"
              ? "回测记录"
              : view === "configure"
                ? "新建回测"
                : view === "progress"
                  ? "回测进度"
                  : "回测结果",
          )}
        </h3>
        {requestedDelivery && (
          <DeliveryNotice
            binding={binding}
            portfolio={Boolean(lockedPortfolio)}
            boundDataMatches={boundDataMatches}
          />
        )}
        <div className="source-actions">
          {view !== "records" && (
            <button disabled={busy} onClick={() => setView("records")}>
              {t("返回回测记录")}
            </button>
          )}
          {view !== "configure" && (
            <button
              className="primary"
              disabled={busy || !taskService?.online}
              onClick={() => {
                navigate("workspace.backtest", { page: "backtest" });
                setView("configure");
                setStep(0);
              }}
            >
              {t("新建回测")}
            </button>
          )}
        </div>
      </div>
      <div className="research-layout backtest-layout">
        {view === "configure" && (
          <section className="research-config" aria-label={t("回测设置")}>
            <h3>{t("回测设置")}</h3>
            <FlowSteps
              labels={[t("选择历史数据"), t("参数与交易规则"), t("确认运行")]}
              current={step}
            />
            <div hidden={step !== 0}>
              <DatasetPicker
                query={query}
                snapshot={snapshot}
                busy={busy}
                trade={trade}
                onDownload={() => navigate("workspace.history")}
                onSelected={() => setConfirmedSource(sourceRequestKey)}
                sourceRequest={
                  workspacePage === "backtest" && workspaceParams?.source_dataset_id
                    ? {
                        id: workspaceParams.source_dataset_id,
                        connection: workspaceParams.connection_id ?? "",
                        request: workspaceParams.selection_id ?? "",
                      }
                    : undefined
                }
              />
              <div className="workflow-actions">
                <button
                  className="primary"
                  disabled={busy || !dataReady}
                  onClick={() => setStep(1)}
                >
                  {t("下一步")}
                </button>
              </div>
            </div>
            <p className="subtle">
              {t(
                "单合约或组合 · 合计最多 200000 根 K 线；委托在本合约的下一根 K 线撮合，交易日结束按数据源结算价结算",
              )}
            </p>
            <form hidden={step !== 1} onSubmit={event => void submit(event)}>
              <fieldset disabled={busy || !dataReady || step !== 1}>
                {/* A fixed-weight portfolio is optional; the rule and its windows
                    come first. */}
                <details className="backtest-options" open={portfolioMode}>
                  <summary>{t("更多选项：固定权重组合")}</summary>
                  <fieldset disabled={Boolean(binding)}>
                    <PortfolioPolicyEditor
                      draft={portfolioEditorDraft}
                      datasets={datasets}
                      eligible={eligiblePortfolio}
                      disabled={Boolean(binding)}
                      onChange={setPortfolioDraft}
                    />
                  </fieldset>
                </details>
                <fieldset disabled={Boolean(binding)}>
                  {!portfolioMode && (
                    <StrategyFields
                      className="research-fields"
                      value={shownStrategy}
                      onChange={setStrategy}
                      compare={!binding}
                      portfolio={!binding}
                    />
                  )}
                </fieldset>
                {ranked < 2 * held && (
                  <p role="alert" className="alert">
                    {t(
                      "排序规则每侧持有 {held} 个，至少需要 {need} 个合约或主力连续；当前选了 {have} 个。",
                      { held, need: 2 * held, have: ranked },
                    )}
                  </p>
                )}
                {unranked && (
                  <p role="alert" className="alert">
                    {unranked}
                  </p>
                )}
                {!portfolioMode && (
                  <details className="subtle">
                    <summary>{t("窗口可用逗号填多个值，生成多个候选并比较")}</summary>
                    <p>
                      {t(
                        "窗口里可以用逗号填多个值，排序规则还可以两个方向都比较；所有组合各成一个候选策略，最多 32 个；规则用不了的组合不算。多个候选要选一种比较方式：留出，只用留出起始日之前的交易日比较，取夏普比率最高的一个回放全部交易日；滚动验证，每一轮用一段训练交易日比较，账户在随后的验证交易日里跟随这一轮夏普比率最高的那个，再整体向后移一轮。",
                      )}
                    </p>
                  </details>
                )}
                <CandidateComparison
                  candidates={candidates}
                  comparison={comparison}
                  setComparison={setComparison}
                  holdoutFrom={holdoutFrom}
                  setHoldoutFrom={setHoldoutFrom}
                  rolling={rolling}
                  setRolling={setRolling}
                  portfolioIncomplete={portfolioDraft.enabled && !binding}
                />
                <AccountLimits parameters={parameters} setParameters={setParameters} />
                <ContractCosts
                  datasets={datasets}
                  series={snapshot?.dataset_series ?? []}
                  drafts={costs}
                  onChange={setCosts}
                />
                <HistoricalMarginChoice
                  busy={busy}
                  supportsSettlementMargins={supportsSettlementMargins}
                  settlementMargins={settlementMargins}
                  setSettlementMargins={setSettlementMargins}
                  historicalMargins={historicalMargins}
                  marginChoice={marginChoice}
                  setMarginChoice={setMarginChoice}
                  invalidSettlementScope={invalidSettlementScope}
                  previewMargins={previewMargins}
                />
                <PerformanceAssumptionFields
                  performanceDraft={performanceDraft}
                  setPerformanceDraft={setPerformanceDraft}
                  validAnnualPeriods={validAnnualPeriods}
                  candidates={candidates}
                  performanceReady={performanceReady}
                />
                <div className="workflow-actions">
                  <button type="button" onClick={() => setStep(0)}>
                    {t("上一步")}
                  </button>
                  <button
                    className="primary"
                    type="submit"
                    disabled={
                      !taskService?.online ||
                      !snapshot?.data?.online ||
                      !datasets.length ||
                      !performanceReady ||
                      !portfolioReady ||
                      candidates.length < 1 ||
                      candidates.length > 32 ||
                      ranked < 2 * held ||
                      !!unranked ||
                      invalidSettlementScope
                    }
                  >
                    {t("下一步")}
                  </button>
                </div>
              </fieldset>
            </form>
            {step === 2 && (
              <form onSubmit={event => void submit(event)}>
                <h3>{t("运行摘要")}</h3>
                <BacktestSummary
                  datasets={datasets}
                  candidates={candidates}
                  comparison={comparison}
                  holdoutFrom={holdoutFrom}
                  rolling={rolling}
                  sides={shownStrategy.sides}
                  fixedPortfolio={fixedPortfolio}
                  parameters={parameters}
                  annualPeriods={annualPeriods}
                  performanceDraft={performanceDraft}
                  taskService={taskService}
                  settlementMargins={settlementMargins}
                  historicalMargins={historicalMargins}
                />
                <p className="subtle">
                  {t("回测只使用历史数据，不向柜台发送委托。提交后可离开此页面。")}
                </p>
                <div className="workflow-actions">
                  <button type="button" disabled={busy} onClick={() => setStep(1)}>
                    {t("上一步")}
                  </button>
                  <button
                    className="primary"
                    disabled={
                      busy ||
                      !taskService?.online ||
                      !snapshot?.data?.online ||
                      !dataReady ||
                      !performanceReady ||
                      !portfolioReady ||
                      invalidSettlementScope
                    }
                  >
                    {t(pendingId ? "确认提交状态" : "开始回测")}
                  </button>
                </div>
              </form>
            )}
            {submitted && (
              <p className="subtle" role="status">
                {t("任务已提交，可在任务中心查看进度。")}
              </p>
            )}
            <MatchingNotes />
          </section>
        )}
        {view !== "configure" && (
          <div className="research-main">
            {view === "progress" && (
              <BacktestProgress
                activeTask={activeTask}
                taskService={taskService}
                snapshot={snapshot}
                busy={busy}
                run={run}
                onResult={id => void showResult(id)}
              />
            )}
            {view === "records" && (
              <TaskRecords
                query={query}
                taskService={taskService}
                busy={busy}
                trade={trade}
                run={run}
                listed={task => task.kind === "backtest"}
                describe={task => task.trading_day}
                onProgress={task =>
                  navigate("workspace.backtest", {
                    page: "task",
                    params: { id: task.id, kind: task.kind },
                  })
                }
                onResult={task => {
                  navigate("workspace.backtest", { page: "backtest" });
                  void showResult(task.id);
                }}
              />
            )}
            {view === "result" && result && result.id === selectedTask && (
              <BacktestResult
                result={result}
                activeTask={activeTask}
                taskService={taskService}
                snapshot={snapshot}
                busy={busy}
                query={query}
                run={run}
                navigate={navigate}
              />
            )}
          </div>
        )}
      </div>
    </ResearchPage>
  );
}
