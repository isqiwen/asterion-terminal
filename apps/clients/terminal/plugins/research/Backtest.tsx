import { FlowSteps } from "../../src/ui/FlowSteps";
import { useEffect } from "react";
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
import { timestamp, type PerformanceFigures } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
import { namespace, ResearchPage, states, t, TaskRecords, useRun, selectedSeries } from "./shared";

// Performance figures are statistics; an absent one was not computed.
const percent = (value: number | null) => (value === null ? "—" : `${(value * 100).toFixed(2)}%`);
const ratio = (value: number | null) => (value === null ? "—" : value.toFixed(2));
function Figures({ label, value }: { label: string; value: PerformanceFigures }) {
  return (
    <section aria-label={label}>
      <div className="research-metrics">
        {[
          [t("总收益率"), percent(value.total_return)],
          [t("年化收益率"), percent(value.annual_return)],
          [t("年化波动率"), percent(value.annual_volatility)],
          [t("夏普比率"), ratio(value.sharpe)],
          [t("最大回撤"), percent(value.max_drawdown)],
          [t("卡玛比率"), ratio(value.calmar)],
          [t("盈利交易日占比"), percent(value.winning_days)],
          [t("交易日数"), value.trading_days],
        ].map(([name, figure]) => (
          <div key={name}>
            <span className="subtle">{name}</span>
            <strong>{figure}</strong>
          </div>
        ))}
      </div>
    </section>
  );
}

/** Strategy backtests: their records, a three-step form, one task's progress and its fixed result. */
export function Backtest({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  workspacePage,
  workspaceParams,
}: TerminalContext) {
  const taskService = snapshot?.task_service;
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
    if (await run("task.result", { id })) {
      setSelectedTask(id);
      setView("result");
    }
  }
  const datasets = snapshot?.datasets ?? [];
  const dataReady =
    datasets.length > 0 && (!sourceRequestKey || confirmedSource === sourceRequestKey);
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

  const destination = JSON.stringify([snapshot?.data?.connection_id, taskService?.connection_id]);
  const [strategy, setStrategy] = useWorkspaceDraft("backtest-strategy", strategyDefaults);
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
  const candidates = strategiesOf(strategy);
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
  const [submitted, setSubmitted] = useWorkspaceDraft(`submitted:${destination}:backtest`, "");
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    "backtest-submission",
    JSON.stringify([
      destination,
      strategy,
      holdoutFrom,
      comparison,
      rolling,
      parameters,
      costs,
      datasets.map(item => item.revision),
    ]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (!dataReady) {
      setStep(0);
      return;
    }
    if (step === 1) {
      setStep(2);
      return;
    }
    const id = pendingId ?? `backtest-${crypto.randomUUID()}`;
    setPendingId(id);
    try {
      const payload = {
        id,
        ...parameters,
        strategies: candidates,
        holdout_from: candidates.length > 1 && comparison === "holdout" ? holdoutFrom : "",
        walk_forward:
          candidates.length > 1 && comparison === "rolling"
            ? {
                training_days: Number(rolling.training),
                validation_days: Number(rolling.validation),
              }
            : null,
        contracts: contractCostRequest(datasets, costs, snapshot?.dataset_series ?? []),
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
  const values = result?.result.equity.map(p => Number(p.equity)) ?? [];
  // Full task results can exceed the JavaScript argument-count limit. The
  // axis shows the extremes as the result states them, not as recomputed
  // numbers.
  let low = values[0] ?? 0;
  let high = low;
  let lowText = result?.result.equity[0]?.equity ?? "";
  let highText = lowText;
  values.forEach((value, index) => {
    if (value < low) {
      low = value;
      lowText = result!.result.equity[index].equity;
    }
    if (value > high) {
      high = value;
      highText = result!.result.equity[index].equity;
    }
  });
  const points = values
    .map(
      (value, index) =>
        `${8 + (index * 624) / Math.max(1, values.length - 1)},${152 - ((value - low) * 144) / Math.max(1, high - low)}`,
    )
    .join(" ");
  return (
    <ResearchPage title={t("策略回测")} taskService={taskService} error={error}>
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
                navigate("workspace.research", { page: "backtest" });
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
                onDownload={() => navigate("workspace.data", { page: "history" })}
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
                <StrategyFields
                  className="research-fields"
                  value={strategy}
                  onChange={setStrategy}
                  compare
                  portfolio
                />
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
                <p className="subtle">
                  {t(
                    "窗口里可以用逗号填多个值，所有组合各成一个候选策略，最多 32 个；规则用不了的组合不算。多个候选要选一种比较方式：留出，只用留出起始日之前的交易日比较，取夏普比率最高的一个回放全部交易日；滚动验证，每一轮用一段训练交易日比较，账户在随后的验证交易日里跟随这一轮夏普比率最高的那个，再整体向后移一轮。",
                  )}
                </p>
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
                    ? t("这些窗口组不出可用的策略。")
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
                      candidates.length < 1 ||
                      candidates.length > 32 ||
                      ranked < 2 * held ||
                      !!unranked
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
                    {strategySides(strategy.sides)}
                  </dd>
                  <dt>{t("初始资金")}</dt>
                  <dd>{parameters.deposit}</dd>
                  <dt>{t("风险限制")}</dt>
                  <dd>
                    {t("单笔数量上限")} {parameters.max_order_quantity} · {t("总持仓量上限")}{" "}
                    {parameters.max_gross_quantity} · {t("在途委托数上限")}{" "}
                    {parameters.max_working_orders}
                  </dd>
                  <dt>{t("运行位置")}</dt>
                  <dd>
                    {taskService?.remote
                      ? `${taskService.host} · ${taskService.service}`
                      : t("本机")}
                  </dd>
                </dl>
                <p className="subtle">
                  {t("回测只使用历史数据，不向柜台发送委托。提交后可离开此页面。")}
                </p>
                <div className="workflow-actions">
                  <button type="button" disabled={busy} onClick={() => setStep(1)}>
                    {t("上一步")}
                  </button>
                  <button
                    className="primary"
                    disabled={busy || !taskService?.online || !snapshot?.data?.online || !dataReady}
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
                        void run("task.action", { id: activeTask.id, action: "cancel" })
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
                          void run("task.action", { id: activeTask.id, action: "retry" })
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
                taskService={taskService}
                busy={busy}
                trade={trade}
                run={run}
                listed={task => task.kind === "backtest"}
                describe={task => task.trading_day}
                onProgress={task =>
                  navigate("workspace.research", {
                    page: "task",
                    params: { id: task.id, kind: task.kind },
                  })
                }
                onResult={task => {
                  navigate("workspace.research", { page: "backtest" });
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
                <Figures label={t("绩效")} value={result.performance} />
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
                    <div className="research-table">
                      <table aria-label={t("滚动验证各轮")}>
                        <thead>
                          <tr>
                            <th>{t("验证起始日")}</th>
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
                                <td>{fold.first_day}</td>
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
                                <td>{validation?.trading_days ?? "—"}</td>
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
                <p className="subtle">
                  {t(
                    "按逐日结算权益计算，是统计值而不是账本数值。年化按这些交易日跨越的日历时间折算，无风险利率取 0；不足 20 个交易日时不计算年化指标。",
                  )}
                </p>
                <h4>{t("按笔统计")}</h4>
                <section aria-label={t("按笔统计")}>
                  <div className="research-metrics">
                    {[
                      [t("平仓笔数"), result.trades.count],
                      [
                        t("盈利笔数占比"),
                        percent(
                          result.trades.count ? result.trades.winning / result.trades.count : null,
                        ),
                      ],
                      [t("平均盈利"), ratio(result.trades.average_win)],
                      [t("平均亏损"), ratio(result.trades.average_loss)],
                      [t("盈亏比"), ratio(result.trades.payoff)],
                    ].map(([name, figure]) => (
                      <div key={name}>
                        <span className="subtle">{name}</span>
                        <strong>{figure}</strong>
                      </div>
                    ))}
                  </div>
                </section>
                <p className="subtle">
                  {t(
                    "一笔是一个合约从开仓到全部平掉的一次持仓，不论分几次成交；盈亏是卖出所得减买入所付再乘合约乘数，未扣手续费。期末还没平掉的持仓不算一笔，主力连续换月时新旧月份各算一笔。",
                  )}
                </p>
                <div className="research-chart">
                  <div className="research-axis" aria-hidden>
                    <span>{highText}</span>
                    <span>{lowText}</span>
                  </div>
                  <svg
                    className="research-equity"
                    viewBox="0 0 640 160"
                    preserveAspectRatio="none"
                    role="img"
                    aria-label={t("权益曲线")}
                  >
                    <polyline
                      points={points}
                      fill="none"
                      stroke="var(--accent)"
                      strokeWidth="2"
                      vectorEffect="non-scaling-stroke"
                    />
                  </svg>
                  <div className="research-range">
                    <span>{timestamp(result.result.equity[0]?.timestamp_ns ?? null)}</span>
                    <span>{timestamp(result.result.equity.at(-1)?.timestamp_ns ?? null)}</span>
                  </div>
                </div>
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
                <ExperimentDetails evidence={result} />
                <details>
                  <summary>{t("结果详情")}</summary>
                  <p>{result.id}</p>
                  <p>
                    {t("数据版本")} <code>{result.result.dataset_revision}</code>
                  </p>
                  <p>{result.result.engine_version}</p>
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
