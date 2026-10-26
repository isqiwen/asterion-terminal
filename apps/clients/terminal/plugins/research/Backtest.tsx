import { taskOwner, isPortfolioStrategyPackage } from "../../src/bridge/client";
import {
  SettlementMarginEditor,
  SettlementMarginDetails,
  SettlementMarginResults,
} from "./SettlementMargins";
import { QuantStatsReport } from "./QuantStatsReport";
import { ResearchExport } from "./ResearchExport";
import { BacktestReuse } from "./BacktestReuse";
import { TargetRejectionEvidence } from "./TargetRejectionEvidence";
import { PortfolioBudget } from "./PortfolioBudget";
import { PortfolioTargetsEvidence } from "./PortfolioTargetsEvidence";
import {
  PortfolioPolicyEditor,
  portfolioPolicy,
  portfolioEligible,
  portfolioPolicyDefaults,
} from "./PortfolioPolicy";
import { AccountReconciliation } from "./AccountReconciliation";
import { TradeStatistics } from "./TradeStatistics";
import { MonthlyContributions } from "./MonthlyContributions";
import { FlowSteps } from "../../src/ui/FlowSteps";
import { useEffect, useMemo, useRef, useState } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  getLocale,
  DatasetPicker,
  ContractCosts,
  contractCostRequest,
  type ContractCostDrafts,
  ErrorNotice,
  asDisplayError,
  StrategyFields,
  strategyDefaults,
  strategiesOf,
  strategyRule,
  strategySides,
  strategySize,
  type TerminalContext,
} from "../contract";
import { timestamp, type SettlementMargin, type HistoricalMargins } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
import type {
  LogicStrategyPackage,
  LogicStrategyDelivery,
  PortfolioStrategyPackage,
  ReferenceStrategyDelivery,
  ResearchPlanBinding,
  PerformanceAssumptions,
} from "../../src/bridge/client";
import { ResearchPlanPicker, ResearchPlanEvidence } from "./ResearchPlan";
import { namespace, ResearchPage, states, t, TaskRecords, useRun, selectedSeries } from "./shared";

import { Figures, percent, ratio } from "./Performance";

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
  const [plan, setPlan] = useWorkspaceDraft<ResearchPlanBinding | null>(
    `backtest-plan:${taskService?.connection_id}`,
    null,
  );
  const [planReady, setPlanReady] = useState(false);
  const planHint = workspaceParams?.plan_id ? JSON.stringify(workspaceParams) : "";
  const [seenPlanHint, setSeenPlanHint] = useWorkspaceDraft(
    `backtest-plan-hint:${taskService?.connection_id}`,
    "",
  );
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
    setView("configure");
    setStep(0);
  }, [planHint, seenPlanHint, setSeenPlanHint, setPlan, setView, setStep]);
  const researchReady = planReady && (!planHint || planHint === seenPlanHint);
  const datasets = snapshot?.datasets ?? [];
  const deliveryQuery = useRef(query);
  deliveryQuery.current = query;
  const requestedDelivery =
    workspacePage === "backtest" ? workspaceParams?.strategy_delivery_id : "";
  const [binding, setBinding] = useState<{
    package: LogicStrategyPackage | PortfolioStrategyPackage;
    delivery: LogicStrategyDelivery | ReferenceStrategyDelivery;
  } | null>(null);
  useEffect(() => {
    let active = true;
    setBinding(null);
    if (!requestedDelivery) return;
    void (async () => {
      const draft = (
        await deliveryQuery.current("research.strategy.delivery.read", { id: requestedDelivery })
      ).research_strategy?.delivery;
      if (!draft) throw new Error(t("固定策略交付草稿不一致"));
      const candidate = (
        await deliveryQuery.current("research.strategy.read", { id: draft.package.id })
      ).research_strategy?.package;
      if (
        !candidate ||
        candidate.object.id !== draft.package.id ||
        candidate.object.version !== draft.package.version
      )
        throw new Error(t("固定策略交付草稿不一致"));
      if (!active) return;
      if (
        candidate.evidence_relation === "logic_trial" &&
        draft.reference_result === null &&
        candidate.logic.result.id === draft.logic_trial.id &&
        candidate.logic.result.version === draft.logic_trial.version
      ) {
        setBinding({ package: candidate, delivery: draft });
      } else if (
        isPortfolioStrategyPackage(candidate) &&
        draft.reference_result !== null &&
        candidate.reference_result.id === draft.reference_result.id &&
        candidate.reference_result.version === draft.reference_result.version &&
        candidate.source_input_sha256 === draft.source_input_sha256
      ) {
        setBinding({ package: candidate, delivery: draft });
      } else {
        throw new Error(t("固定策略交付草稿不一致"));
      }
      setSelectedTask(draft.target_task_id);
      setView("configure");
      setStep(0);
    })().catch(reason => {
      if (active) setError(asDisplayError(reason));
    });
    return () => {
      active = false;
    };
  }, [requestedDelivery, taskService?.connection_id, setError, setSelectedTask, setView, setStep]);
  const boundDefinition = binding?.package.definition;
  const boundContracts =
    boundDefinition?.portfolio?.contracts ??
    (boundDefinition?.contract ? [boundDefinition.contract] : []);
  const boundDataMatches =
    !requestedDelivery ||
    Boolean(
      binding &&
      boundContracts.length > 0 &&
      datasets.length === boundContracts.length &&
      !snapshot?.dataset_series?.length &&
      boundContracts.every((contract, index) => {
        const actual = datasets[index];
        return (
          actual.venue === contract.venue &&
          actual.symbol === contract.symbol &&
          actual.interval_minutes === binding.package.definition.interval_minutes &&
          (!boundDefinition?.portfolio ||
            (
              [
                "currency",
                "multiplier",
                "price_increment",
                "quantity_increment",
                "delivery_month",
                "product",
              ] as const
            ).every(key => actual.contract[key] === contract[key]))
        );
      }),
    );

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
  const [strategy, setStrategy] = useWorkspaceDraft("backtest-strategy", strategyDefaults);
  const [portfolioDraft, setPortfolioDraft] = useWorkspaceDraft(
    "backtest-portfolio-policy",
    portfolioPolicyDefaults,
  );
  const eligiblePortfolio = portfolioEligible(datasets, Boolean(snapshot?.dataset_series?.length));
  const lockedPortfolio = boundDefinition?.portfolio?.strategy;
  const portfolioMode = Boolean(lockedPortfolio) || (!binding && portfolioDraft.enabled);
  const fixedPortfolio = binding
    ? (lockedPortfolio ?? null)
    : portfolioPolicy(portfolioDraft, datasets, eligiblePortfolio);
  const portfolioEditorDraft = lockedPortfolio
    ? {
        enabled: true,
        weights: Object.fromEntries(
          lockedPortfolio.rule.members.map(m => [`${m.venue}/${m.symbol}`, m.weight]),
        ),
        gross: lockedPortfolio.rule.gross_weight_limit,
        net: lockedPortfolio.rule.net_weight_limit,
        rebalance: String(lockedPortfolio.rule.rebalance),
      }
    : portfolioDraft;
  const portfolioReady = Boolean(binding) || !portfolioDraft.enabled || fixedPortfolio !== null;
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
  const fixed = boundDefinition?.portfolio?.strategy ?? boundDefinition?.strategy;
  const shownStrategy =
    fixed && fixed.rule.kind === "moving_average" && "quantity" in fixed
      ? {
          ...strategy,
          kind: "moving_average" as const,
          quantity: fixed.quantity,
          sides: fixed.sides,
          fast: String(fixed.rule.fast),
          slow: String(fixed.rule.slow),
        }
      : strategy;
  const candidates = binding
    ? fixed
      ? [fixed]
      : []
    : portfolioMode
      ? fixedPortfolio
        ? [fixedPortfolio]
        : []
      : strategiesOf(strategy);
  const [performanceDraft, setPerformanceDraft] = useWorkspaceDraft(
    `backtest-performance:${JSON.stringify([destination, datasets.map(item => item.revision)])}`,
    { annual: "", zeroRiskFree: false },
  );
  const annualPeriods =
    performanceDraft.annual.trim() === "" ? null : Number(performanceDraft.annual);
  const validAnnualPeriods =
    annualPeriods === null ||
    (Number.isFinite(annualPeriods) && annualPeriods > 0 && annualPeriods <= 366);
  const performanceReady =
    validAnnualPeriods &&
    (candidates.length < 2 || (annualPeriods !== null && performanceDraft.zeroRiskFree));
  const performanceAssumptions: PerformanceAssumptions | undefined =
    validAnnualPeriods && (annualPeriods !== null || performanceDraft.zeroRiskFree)
      ? {
          annual_periods: annualPeriods,
          risk_free: performanceDraft.zeroRiskFree ? "explicit_zero" : "unconfigured",
        }
      : undefined;
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
      plan,
    ]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (!dataReady) {
      setStep(0);
      return;
    }
    if (!researchReady || !performanceReady || !portfolioReady) {
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
        ...(plan ? { research_plan: plan } : {}),
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
  const equitySeries = useMemo(
    () => ({
      kind: "line" as const,
      points: (result?.result.equity ?? []).map(point => ({
        timestamp_ns: point.timestamp_ns,
        price: point.equity,
      })),
    }),
    [result?.result.equity],
  );
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
          <section aria-label={t(lockedPortfolio ? "固定组合交付回测" : "固定逻辑交付回测")}>
            <p>
              {binding
                ? `${binding.package.name} · ${binding.delivery.target_task_id}`
                : t("正在读取固定策略交付")}
            </p>
            <p>
              {t(
                lockedPortfolio
                  ? "全部合约、权重、预算和调仓间隔来自固定包；请显式确认本次Data版本、资金、费用与风险。"
                  : "规则、数量、方向和周期来自固定包；请显式确认本次数据、资金、费用与风险。",
              )}
            </p>
            {!boundDataMatches && (
              <p role="alert">
                {t(
                  lockedPortfolio
                    ? "所选数据需按原顺序匹配固定包的全部合约、单位和周期"
                    : "所选数据需匹配固定包的单合约和周期",
                )}
              </p>
            )}
          </section>
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
                {/* Research plans and fixed-weight portfolios are optional; the
                    rule and its windows come first. */}
                <details className="backtest-options" open={Boolean(plan) || portfolioMode}>
                  <summary>{t("更多选项：研究计划、固定权重组合")}</summary>
                  <ResearchPlanPicker
                    snapshot={snapshot}
                    query={query}
                    binding={plan}
                    onChange={setPlan}
                    onReady={setPlanReady}
                  />
                  <p className="subtle">
                    {t("研究计划记录原问题与判断标准，不替代时间划分或证明未提前见过样本。")}
                  </p>
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
                {candidates.length > 1 && (
                  <div className="research-fields">
                    <label>
                      {t("比较方式")}
                      <select
                        aria-label={t("比较方式")}
                        value={comparison}
                        onChange={event =>
                          setComparison(event.target.value as "holdout" | "rolling")
                        }
                      >
                        <option value="holdout">{t("留出")}</option>
                        <option value="rolling">{t("滚动验证")}</option>
                      </select>
                    </label>
                    {comparison === "holdout" ? (
                      <label>
                        {t("留出起始日")}
                        <input
                          aria-label={t("留出起始日")}
                          type="date"
                          required
                          value={holdoutFrom}
                          onChange={event => setHoldoutFrom(event.target.value)}
                        />
                      </label>
                    ) : (
                      (
                        [
                          ["training", "训练交易日数", 20],
                          ["validation", "每轮验证交易日数", 1],
                        ] as const
                      ).map(([key, label, least]) => (
                        <label key={key}>
                          {t(label)}
                          <input
                            aria-label={t(label)}
                            type="number"
                            min={least}
                            step="1"
                            required
                            value={rolling[key]}
                            onChange={event =>
                              setRolling({ ...rolling, [key]: event.target.value })
                            }
                          />
                        </label>
                      ))
                    )}
                  </div>
                )}
                <p className="subtle" role="status">
                  {candidates.length === 0
                    ? t(
                        portfolioDraft.enabled && !binding
                          ? "组合政策不完整，尚不能进入运行确认。"
                          : "这些窗口组不出可用的策略。",
                      )
                    : candidates.length > 32
                      ? t("候选策略 {n} 个，超过 32 个。", { n: candidates.length })
                      : candidates.length > 1
                        ? comparison === "holdout"
                          ? t(
                              "候选策略 {n} 个；留出起始日之前至少要有 20 个交易日，起始日不是交易日时取其后的第一个交易日。",
                              { n: candidates.length },
                            )
                          : t(
                              "候选策略 {n} 个；训练交易日至少 20 个且少于全部交易日数，第一轮验证开始之前账户不持仓，最后一轮验证取剩下的交易日，最多 100 轮。",
                              { n: candidates.length },
                            )
                        : t("一个策略，不做比较。")}
                </p>
                <div className="research-fields">
                  {(
                    [
                      ["deposit", "初始资金"],
                      ["max_order_quantity", "单笔数量上限"],
                      ["max_gross_quantity", "总持仓量上限"],
                      ["max_working_orders", "在途委托数上限"],
                    ] as const
                  ).map(([key, label]) => (
                    <label key={key}>
                      {t(label)}
                      <input
                        aria-label={t(label)}
                        inputMode="decimal"
                        required
                        value={parameters[key]}
                        onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                      />
                    </label>
                  ))}
                </div>
                <ContractCosts
                  datasets={datasets}
                  series={snapshot?.dataset_series ?? []}
                  drafts={costs}
                  onChange={setCosts}
                />
                <section aria-label={t("选择历史保证金")}>
                  <p>
                    {t(
                      "先在数据工作台下载上期所历史保证金，再预览并确认。首批仅支持 AU2512 的 2025-10-20 至 2025-10-22。",
                    )}
                  </p>
                  <button
                    type="button"
                    disabled={busy || !supportsSettlementMargins || settlementMargins.length > 0}
                    onClick={() => void previewMargins()}
                  >
                    {t("读取历史保证金快照")}
                  </button>
                  {historicalMargins && (
                    <>
                      <SettlementMarginDetails historical={historicalMargins} />
                      <button type="button" onClick={() => setMarginChoice(null)}>
                        {t("取消使用历史保证金")}
                      </button>
                    </>
                  )}
                  {invalidSettlementScope && marginChoice && (
                    <p role="alert">{t("历史保证金选择已不适用，请重新预览或取消使用。")}</p>
                  )}
                </section>
                {!historicalMargins && (
                  <SettlementMarginEditor
                    events={settlementMargins}
                    supported={supportsSettlementMargins}
                    onChange={setSettlementMargins}
                  />
                )}
                <fieldset>
                  <legend>{t("绩效计算方法假设")}</legend>
                  <p>
                    {t(
                      "年化期数由本次日结算方法明确选择，不从样本跨度推断；未配置时保留缺项原因。比较或走步按夏普选优，须配置年期数并明确零无风险假设。",
                    )}
                  </p>
                  <label>
                    {t("年期数A（日结算）")}
                    <input
                      type="number"
                      min="0"
                      max="366"
                      step="any"
                      value={performanceDraft.annual}
                      onChange={event =>
                        setPerformanceDraft({ ...performanceDraft, annual: event.target.value })
                      }
                    />
                  </label>
                  <label>
                    <input
                      type="checkbox"
                      checked={performanceDraft.zeroRiskFree}
                      onChange={event =>
                        setPerformanceDraft({
                          ...performanceDraft,
                          zeroRiskFree: event.target.checked,
                        })
                      }
                    />
                    {t("明确采用零无风险年收益假设")}
                  </label>
                  {!validAnnualPeriods && (
                    <p role="alert">{t("年期数A须为大于0且不超过366的日结算年期数。")}</p>
                  )}
                  {candidates.length > 1 && !performanceReady && (
                    <p role="status">{t("候选评分缺少明确年期数或零无风险选择。")}</p>
                  )}
                </fieldset>
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
                      !researchReady ||
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
                <dl className="research-summary">
                  <dt>{t("合约")}</dt>
                  <dd>{datasets.map(d => `${d.venue} · ${d.symbol}`).join(" + ")}</dd>
                  <dt>{t("策略")}</dt>
                  <dd>
                    {candidates.length === 1
                      ? strategyRule(candidates[0])
                      : comparison === "holdout"
                        ? t("{n} 个候选，留出自 {day} 起", {
                            n: candidates.length,
                            day: holdoutFrom,
                          })
                        : t(
                            "{n} 个候选，滚动验证：训练 {training} 个交易日，每轮验证 {validation} 个",
                            {
                              n: candidates.length,
                              ...rolling,
                            },
                          )}{" "}
                    {candidates.length > 0 && <>· {strategySize(candidates[0])} </>}·{" "}
                    {strategySides(candidates[0]?.sides ?? shownStrategy.sides)}
                  </dd>
                  {fixedPortfolio && (
                    <>
                      <dt>{t("固定组合名义政策")}</dt>
                      <dd>
                        <p>
                          {t("组合总绝对名义权重预算")}：{fixedPortfolio.rule.gross_weight_limit} ·{" "}
                          {t("组合净名义权重预算")}：{fixedPortfolio.rule.net_weight_limit} ·{" "}
                          {t("固定调仓间隔")}：{fixedPortfolio.rule.rebalance}
                        </p>
                        <ul>
                          {fixedPortfolio.rule.members.map(m => (
                            <li key={`${m.venue}/${m.symbol}`}>
                              {m.venue}/{m.symbol}：{m.weight}
                            </li>
                          ))}
                        </ul>
                      </dd>
                    </>
                  )}
                  <dt>{t("初始资金")}</dt>
                  <dd>{parameters.deposit}</dd>
                  <dt>{t("风险限制")}</dt>
                  <dd>
                    {t("单笔数量上限")} {parameters.max_order_quantity} · {t("总持仓量上限")}{" "}
                    {parameters.max_gross_quantity} · {t("在途委托数上限")}{" "}
                    {parameters.max_working_orders}
                  </dd>
                  <dt>{t("绩效计算方法假设")}</dt>
                  <dd>
                    {t("年期数A（日结算）")}: {annualPeriods ?? t("未配置")} ·{" "}
                    {performanceDraft.zeroRiskFree ? t("明确零无风险") : t("无风险收益未配置")}
                  </dd>
                  <dt>{t("运行位置")}</dt>
                  <dd>
                    {taskService?.remote
                      ? `${taskService.host} · ${taskService.service}`
                      : t("本机")}
                  </dd>
                </dl>
                <SettlementMarginDetails
                  events={settlementMargins}
                  historical={historicalMargins}
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
                      !researchReady ||
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
            <details>
              <summary>{t("撮合说明")}</summary>
              <p>
                {t(
                  "信号按后续成交撮合，委托不跨时段；每日按填写的结算价结算，持仓和均线历史续接，不强制平仓。",
                )}
              </p>
              <p>
                {t(
                  "时段结束时刻不包含在内，结算发生在当日末时段结束时。系统不推断节假日；保证金、手续费和结算来源由你提供。",
                )}
              </p>
            </details>
          </section>
        )}
        {view !== "configure" && (
          <div className="research-main">
            {view === "progress" && (
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
                      onClick={() => void showResult(activeTask.id)}
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
                  {activeTask &&
                    ["failed", "cancelled", "interrupted"].includes(activeTask.state) && (
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
                      <p className="subtle">
                        {t("前段结束时权益已不为正，留出段没有收益率可言。")}
                      </p>
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
                                    fold.selected === null
                                      ? null
                                      : fold.candidates[fold.selected].sharpe,
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
                <PortfolioBudget
                  key={`budget-${result.id}`}
                  result={result}
                  query={query}
                  busy={busy}
                />
                <MonthlyContributions
                  key={`months-${result.id}`}
                  months={result.performance.by_month}
                />
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
                          {["交易日", "合约", "结算价", "结算后持仓", "期末权益", "手续费"].map(
                            label => (
                              <th key={label}>{t(label)}</th>
                            ),
                          )}
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
                  <p className="subtle">
                    {t("手续费为累计值，权益曲线包含每根 K 线的估值和日终结算。")}
                  </p>
                </details>
                <SettlementMarginResults settlements={result.result.settlements} />
                <ResearchPlanEvidence binding={result.task.research_plan} query={query} />
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
            )}
          </div>
        )}
      </div>
    </ResearchPage>
  );
}
