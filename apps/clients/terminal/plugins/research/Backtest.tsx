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
  type TerminalContext,
} from "../contract";
import { timestamp } from "../../src/bridge/client";
import { ExperimentDetails } from "./ExperimentDetails";
import { namespace, ResearchPage, states, t, TaskRecords, useRun, positionSides } from "./shared";

// Performance figures are statistics; an absent one was not computed.
const percent = (value: number | null) => (value === null ? "—" : `${(value * 100).toFixed(2)}%`);
const ratio = (value: number | null) => (value === null ? "—" : value.toFixed(2));

/** SMA backtests: their records, a three-step form, one task's progress and its fixed result. */
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
  const [parameters, setParameters] = useWorkspaceDraft("backtest-parameters", {
    fast: "5",
    slow: "20",
    quantity: "1",
    sides: "both",
    deposit: "",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [costs, setCosts] = useWorkspaceDraft<ContractCostDrafts>("contract-costs", {});
  const [submitted, setSubmitted] = useWorkspaceDraft(`submitted:${destination}:backtest`, "");
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    "backtest-submission",
    JSON.stringify([destination, parameters, costs, datasets.map(item => item.revision)]),
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
        contracts: contractCostRequest(datasets, costs, snapshot?.dataset_series ?? []),
        fast: Number(parameters.fast),
        slow: Number(parameters.slow),
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
    <ResearchPage title={t("均线回测")} taskService={taskService} error={error}>
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
                <div className="research-fields">
                  {(
                    [
                      ["fast", "快均线"],
                      ["slow", "慢均线"],
                      ["quantity", "目标手数"],
                    ] as const
                  ).map(([key, label]) => (
                    <label key={key}>
                      {t(label)}
                      <input
                        aria-label={t(label)}
                        type="number"
                        min={key === "slow" ? 2 : 1}
                        max={key === "quantity" ? undefined : 10000}
                        step="1"
                        required
                        value={parameters[key]}
                        onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                      />
                    </label>
                  ))}
                  <label>
                    {t("持仓方向")}
                    <select
                      aria-label={t("持仓方向")}
                      value={parameters.sides}
                      onChange={e => setParameters({ ...parameters, sides: e.target.value })}
                    >
                      {Object.entries(positionSides).map(([value, label]) => (
                        <option key={value} value={value}>
                          {t(label)}
                        </option>
                      ))}
                    </select>
                  </label>
                </div>
                <p className="subtle">
                  {t(
                    "快线高于慢线时持有目标手数的多单，低于时持有空单；不允许的方向空仓。反手时先平仓，平掉之后的下一根 K 线再开仓。",
                  )}
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
                    disabled={!taskService?.online || !snapshot?.data?.online || !datasets.length}
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
                  <dt>{t("均线参数")}</dt>
                  <dd>
                    {parameters.fast} / {parameters.slow} · {t("目标手数")} {parameters.quantity}
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
                  {activeTask?.instrument} · {t("均线回测")}
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
                <section aria-label={t("绩效")}>
                  <div className="research-metrics">
                    {[
                      [t("总收益率"), percent(result.performance.total_return)],
                      [t("年化收益率"), percent(result.performance.annual_return)],
                      [t("年化波动率"), percent(result.performance.annual_volatility)],
                      [t("夏普比率"), ratio(result.performance.sharpe)],
                      [t("最大回撤"), percent(result.performance.max_drawdown)],
                      [t("卡玛比率"), ratio(result.performance.calmar)],
                      [t("盈利交易日占比"), percent(result.performance.winning_days)],
                      [t("交易日数"), result.performance.trading_days],
                    ].map(([label, value]) => (
                      <div key={label}>
                        <span className="subtle">{label}</span>
                        <strong>{value}</strong>
                      </div>
                    ))}
                  </div>
                  <p className="subtle">
                    {t(
                      "按逐日结算权益计算，是统计值而不是账本数值。年化按这些交易日跨越的日历时间折算，无风险利率取 0；不足 20 个交易日时不计算年化指标。",
                    )}
                  </p>
                </section>
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
