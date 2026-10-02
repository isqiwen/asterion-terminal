import { FlowSteps } from "../../src/ui/FlowSteps";
import { useState, useEffect } from "react";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  translate,
  getLocale,
  DatasetPicker,
  ContractCosts,
  contractCostRequest,
  type ContractCostDrafts,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
  type MessageValues,
} from "../contract";
import { timestamp, type TerminalCommand } from "../../src/bridge/client";
import "./research.css";
import { DailyFactorForm, DailyFactorResults } from "./DailyFactor";
import { FactorResults } from "./FactorResults";
import { ExperimentDetails } from "./ExperimentDetails";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
const states: Record<string, string> = {
  queued: "排队中",
  running: "运行中",
  cancel_requested: "正在取消",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
export function Panel({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  workspacePage,
  workspaceParams,
}: TerminalContext) {
  const research = snapshot?.research;
  const [view, setView] = useWorkspaceDraft<"records" | "configure" | "progress" | "result">(
    `backtest-view:${research?.connection_id}`,
    "records",
  );
  const [step, setStep] = useWorkspaceDraft("backtest-step", 0);
  const [received, setReceived] = useWorkspaceDraft("backtest-source-request", "");
  const [confirmedSource, setConfirmedSource] = useWorkspaceDraft("backtest-source-confirmed", "");
  const sourceRequestKey =
    workspacePage === "backtest" && workspaceParams?.selection_id
      ? JSON.stringify([workspaceParams.selection_id, research?.connection_id])
      : "";
  const [selectedTask, setSelectedTask] = useWorkspaceDraft(
    `backtest-task:${research?.connection_id}`,
    "",
  );
  const activeTask = research?.tasks.find(task => task.id === selectedTask);
  async function showResult(id: string) {
    if (await run("research.result", { id })) {
      setSelectedTask(id);
      setView("result");
    }
  }
  const datasets = snapshot?.datasets ?? [];
  // Factor analysis studies exactly one contract.
  const data = datasets.length === 1 ? datasets[0] : undefined;
  const dataReady =
    datasets.length > 0 && (!sourceRequestKey || confirmedSource === sourceRequestKey);
  const [mode, setMode] = useWorkspaceDraft<"backtest" | "factor" | "daily_factor">("mode", () =>
    snapshot?.research_result?.kind === "daily_factor"
      ? "daily_factor"
      : snapshot?.research_result?.kind === "factor"
        ? "factor"
        : "backtest",
  );
  useEffect(() => {
    if (
      workspacePage === "backtest" &&
      workspaceParams?.selection_id &&
      received !== JSON.stringify([workspaceParams.selection_id, research?.connection_id])
    ) {
      setReceived(JSON.stringify([workspaceParams.selection_id, research?.connection_id]));
      setMode("backtest");
      setView("configure");
      setStep(0);
    }
  }, [
    workspacePage,
    workspaceParams,
    research?.connection_id,
    received,
    setReceived,
    setView,
    setStep,
    setMode,
  ]);

  useEffect(() => {
    if (["backtest", "factor", "daily_factor"].includes(workspacePage ?? ""))
      setMode(workspacePage as "backtest" | "factor" | "daily_factor");
  }, [workspacePage, workspaceParams, setMode]);
  useEffect(() => {
    if (workspacePage !== "task" || !workspaceParams?.id) return;
    if (workspaceParams.kind === "backtest") {
      setMode("backtest");
      setSelectedTask(workspaceParams.id);
      setView("progress");
    } else if (workspaceParams.kind === "factor" || workspaceParams.kind === "daily_factor") {
      setMode(workspaceParams.kind);
    }
  }, [workspacePage, workspaceParams, setMode, setSelectedTask, setView]);

  const [factorParameters, setFactorParameters] = useWorkspaceDraft("factor", {
    lookback: "5",
    horizon: "1",
    evaluation: "full_sample",
    split: "",
    training: "",
    validation: "",
  });
  const destination = JSON.stringify([research?.connection_id]);
  const [parameters, setParameters] = useWorkspaceDraft("parameters", {
    fast: "5",
    slow: "20",
    quantity: "1",
    deposit: "",
    max_order_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
  });
  const [costs, setCosts] = useWorkspaceDraft<ContractCostDrafts>("contract-costs", {});
  const [error, setError] = useState<DisplayError>("");
  const [submitted, setSubmitted] = useWorkspaceDraft(`submitted:${destination}:${mode}`, "");
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    "submission",
    JSON.stringify([
      destination,
      mode,
      factorParameters,
      parameters,
      costs,
      datasets.map(item => item.revision),
    ]),
  );
  async function run(method: TerminalCommand, params: Record<string, unknown> = {}) {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (mode === "backtest" && !dataReady) {
      setStep(0);
      return;
    }
    if (mode === "backtest" && step === 1) {
      setStep(2);
      return;
    }
    const id = pendingId ?? `${mode}-${crypto.randomUUID()}`;
    setPendingId(id);
    try {
      const payload =
        mode === "backtest"
          ? {
              id,
              ...parameters,
              contracts: contractCostRequest(datasets, costs),
              fast: Number(parameters.fast),
              slow: Number(parameters.slow),
            }
          : {
              id,
              lookbacks: factorParameters.lookback.split(",").map(value => Number(value.trim())),
              horizon: Number(factorParameters.horizon),
              evaluation:
                factorParameters.evaluation === "full_sample"
                  ? { mode: "full_sample" }
                  : factorParameters.evaluation === "walk_forward"
                    ? {
                        mode: "walk_forward",
                        training_events: Number(factorParameters.training),
                        validation_events: Number(factorParameters.validation),
                      }
                    : { mode: "holdout", split_index: Number(factorParameters.split) },
            };
      if (await run(mode === "backtest" ? "research.submit" : "research.factor.submit", payload)) {
        setSubmitted(id);
        setPendingId(null);
        if (mode === "backtest") {
          setSelectedTask(id);
          setView("progress");
        }
      }
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  const result = snapshot?.research_result?.kind === "backtest" ? snapshot.research_result : null;
  const factorResult =
    snapshot?.research_result?.kind === "factor" ? snapshot.research_result : null;
  const values = result?.result.equity.map(p => Number(p.equity)) ?? [];
  // Full research results can exceed the JavaScript argument-count limit.
  let low = values[0] ?? 0;
  let high = low;
  for (const value of values) {
    low = Math.min(low, value);
    high = Math.max(high, value);
  }
  const points = values
    .map(
      (value, index) =>
        `${8 + (index * 624) / Math.max(1, values.length - 1)},${152 - ((value - low) * 144) / Math.max(1, high - low)}`,
    )
    .join(" ");
  return (
    <section className="research-workspace" aria-label={t("期货研究")}>
      <div className="panel-heading">
        <h2>
          {t(mode === "backtest" ? "均线回测" : mode === "daily_factor" ? "日线因子" : "动量因子")}
        </h2>
        <span className="panel-spacer" />
        {research && (
          <span className="subtle">
            {research.remote ? `${research.host} · ${research.service}` : t("本机")}
          </span>
        )}
        <span role="status">{research?.online ? t("数据服务已连接") : t("数据服务未连接")}</span>
      </div>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.research" />
        </p>
      )}
      {mode === "backtest" && (
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
                disabled={busy || !research?.online}
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
      )}
      <div className={`research-layout ${mode === "backtest" ? "backtest-layout" : ""}`}>
        {(mode !== "backtest" || view === "configure") && (
          <section
            className="research-config"
            aria-label={t(mode === "backtest" ? "回测设置" : "因子设置")}
          >
            {mode === "daily_factor" ? (
              <DailyFactorForm
                query={query}
                snapshot={snapshot}
                busy={busy}
                navigate={navigate}
                run={run}
                workspaceParams={workspaceParams}
              />
            ) : (
              <>
                <h3>{t(mode === "backtest" ? "回测设置" : "因子设置")}</h3>
                {mode === "backtest" && (
                  <FlowSteps
                    labels={[t("选择历史数据"), t("参数与交易规则"), t("确认运行")]}
                    current={step}
                  />
                )}
                <div hidden={mode === "backtest" && step !== 0}>
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
                  {mode === "backtest" && (
                    <div className="workflow-actions">
                      <button
                        className="primary"
                        disabled={busy || !dataReady}
                        onClick={() => setStep(1)}
                      >
                        {t("下一步")}
                      </button>
                    </div>
                  )}
                </div>
                <p className="subtle">
                  {t(
                    mode === "backtest"
                      ? "单合约或组合 · 合计最多 200000 根 K 线；委托在本合约的下一根 K 线撮合，交易日结束按数据源结算价结算"
                      : "单合约 · 按 K 线收盘价计算",
                  )}
                </p>
                {mode !== "backtest" && datasets.length > 1 && (
                  <p role="alert" className="alert">
                    {t("因子分析只研究一个合约，请只保留一个数据集。")}
                  </p>
                )}
                <form
                  hidden={mode === "backtest" && step !== 1}
                  onSubmit={event => void submit(event)}
                >
                  <fieldset
                    disabled={busy || (mode === "backtest" ? !dataReady || step !== 1 : !data)}
                  >
                    {mode === "backtest" ? (
                      <>
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
                                onChange={e =>
                                  setParameters({ ...parameters, [key]: e.target.value })
                                }
                              />
                            </label>
                          ))}
                        </div>
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
                                onChange={e =>
                                  setParameters({ ...parameters, [key]: e.target.value })
                                }
                              />
                            </label>
                          ))}
                        </div>
                        <ContractCosts datasets={datasets} drafts={costs} onChange={setCosts} />
                      </>
                    ) : (
                      <>
                        <div className="research-fields">
                          {(
                            [
                              ["lookback", "回看 K 线数"],
                              ["horizon", "未来收益 K 线数"],
                            ] as const
                          ).map(([key, label]) => (
                            <label key={key}>
                              {t(label)}
                              <input
                                aria-label={t(label)}
                                type={key === "lookback" ? "text" : "number"}
                                inputMode={key === "lookback" ? "text" : "numeric"}
                                pattern={
                                  key === "lookback"
                                    ? "[ ]*[1-9][0-9]*[ ]*(,[ ]*[1-9][0-9]*[ ]*)*"
                                    : undefined
                                }
                                min="1"
                                max="10000"
                                step="1"
                                required
                                value={factorParameters[key]}
                                onChange={e =>
                                  setFactorParameters({
                                    ...factorParameters,
                                    [key]: e.target.value,
                                  })
                                }
                              />
                            </label>
                          ))}
                        </div>
                        <label>
                          {t("评价方式")}
                          <select
                            aria-label={t("评价方式")}
                            value={factorParameters.evaluation}
                            onChange={e =>
                              setFactorParameters({
                                ...factorParameters,
                                evaluation: e.target.value,
                              })
                            }
                          >
                            <option value="full_sample">{t("全样本评价")}</option>
                            <option value="holdout">{t("时间留出评价")}</option>
                            <option value="walk_forward">{t("滚动验证")}</option>
                          </select>
                        </label>
                        {factorParameters.evaluation === "holdout" && (
                          <label>
                            {t("前段 K 线数")}
                            <input
                              aria-label={t("前段 K 线数")}
                              type="number"
                              min="1"
                              max={data ? data.count - 1 : 9999}
                              step="1"
                              required
                              value={factorParameters.split}
                              onChange={e =>
                                setFactorParameters({ ...factorParameters, split: e.target.value })
                              }
                            />
                          </label>
                        )}
                        {factorParameters.evaluation === "walk_forward" && (
                          <>
                            <div className="research-fields">
                              {(
                                [
                                  ["training", "训练 K 线数"],
                                  ["validation", "每轮验证 K 线数"],
                                ] as const
                              ).map(([key, label]) => (
                                <label key={key}>
                                  {t(label)}
                                  <input
                                    aria-label={t(label)}
                                    type="number"
                                    min="1"
                                    max="10000"
                                    step="1"
                                    required
                                    value={factorParameters[key]}
                                    onChange={e =>
                                      setFactorParameters({
                                        ...factorParameters,
                                        [key]: e.target.value,
                                      })
                                    }
                                  />
                                </label>
                              ))}
                            </div>
                            <p className="subtle">
                              {t("固定训练窗口逐轮前移，验证段须完整覆盖剩余数据，共 2–16 轮。")}
                            </p>
                          </>
                        )}
                        <p className="subtle">
                          {t("多个窗口按递增顺序用逗号分隔，最多 32 个，需使用留出或滚动验证。")}
                        </p>
                        <p className="subtle">
                          {t("每段至少 30 个有效样本；跨分界标签不参与评价。")}
                        </p>
                      </>
                    )}
                    {mode === "backtest" && (
                      <button type="button" onClick={() => setStep(0)}>
                        {t("上一步")}
                      </button>
                    )}
                    <button
                      className="primary"
                      type="submit"
                      disabled={
                        !research?.online || (mode === "backtest" ? !datasets.length : !data)
                      }
                    >
                      {mode === "backtest"
                        ? t("下一步")
                        : pendingId
                          ? t("确认提交状态")
                          : t("开始分析")}
                    </button>
                  </fieldset>
                </form>
                {mode === "backtest" && step === 2 && (
                  <form className="creation-review" onSubmit={event => void submit(event)}>
                    <h3>{t("运行摘要")}</h3>
                    <p>{datasets.map(d => `${d.venue} · ${d.symbol}`).join(" + ")}</p>
                    <p>
                      {t("均线参数")}：{parameters.fast} / {parameters.slow} · {t("目标手数")}：
                      {parameters.quantity}
                    </p>
                    <p>
                      {t("初始资金")}：{parameters.deposit} · {t("单笔数量上限")}：
                      {parameters.max_order_quantity}
                    </p>
                    <p>
                      {t("运行位置")}：
                      {research?.remote ? `${research.host} · ${research.service}` : t("本机")}
                    </p>
                    <p className="subtle">
                      {t("回测只使用历史数据，不向柜台发送委托。提交后可离开此页面。")}
                    </p>
                    <div className="workflow-actions">
                      <button type="button" disabled={busy} onClick={() => setStep(1)}>
                        {t("上一步")}
                      </button>
                      <button
                        className="primary"
                        disabled={busy || !research?.online || !dataReady}
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
                {mode === "backtest" && (
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
                )}
                {mode === "factor" && (
                  <details>
                    <summary>{t("计算说明")}</summary>
                    <p>
                      {t(
                        "动量只使用过去价格，留出段可使用此前历史预热。未来标签不进入因子，跨分界标签剔除。留出评价需事先固定参数；反复查看留出结果后调参不能证明样本外有效。",
                      )}
                    </p>
                  </details>
                )}
              </>
            )}
          </section>
        )}
        {(mode !== "backtest" || view !== "configure") && (
          <div className="research-main">
            {mode === "backtest" && view === "progress" && (
              <section className="research-progress" aria-label={t("回测进度")}>
                <h3>{activeTask?.instrument || t("等待任务状态")}</h3>
                <p role="status">
                  {t(activeTask ? states[activeTask.state] : "等待任务状态")}
                  {!research?.online && ` · ${t("最后确认状态")}`}
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
                    <ErrorNotice error={activeTask.error} namespace="asterion.terminal.research" />
                  </p>
                )}
                <div className="source-actions">
                  {activeTask?.state === "succeeded" && (
                    <button
                      className="primary"
                      disabled={busy || !research?.online}
                      onClick={() => void showResult(activeTask.id)}
                    >
                      {t("查看结果")}
                    </button>
                  )}
                  {activeTask && ["queued", "running"].includes(activeTask.state) && (
                    <button
                      disabled={busy || !research?.online}
                      onClick={() =>
                        void run("research.action", { id: activeTask.id, action: "cancel" })
                      }
                    >
                      {t("取消回测")}
                    </button>
                  )}
                  {activeTask &&
                    ["failed", "cancelled", "interrupted"].includes(activeTask.state) && (
                      <button
                        disabled={busy || !research?.online}
                        onClick={() =>
                          void run("research.action", { id: activeTask.id, action: "retry" })
                        }
                      >
                        {t("重新运行")}
                      </button>
                    )}
                </div>
                <p className="subtle">{t("任务已提交，可在任务中心查看进度。")}</p>
              </section>
            )}
            {(mode !== "backtest" || view === "records") && (
              <section aria-label={t("研究任务")}>
                <h3>{t("研究任务")}</h3>
                {!research?.tasks.some(task =>
                  mode === "backtest"
                    ? task.kind === "backtest"
                    : task.kind === "backtest" ||
                      task.kind === "factor" ||
                      task.kind === "daily_factor",
                ) ? (
                  <p className="research-empty">{t("暂无研究任务")}</p>
                ) : (
                  <div className="research-table">
                    <table>
                      <thead>
                        <tr>
                          <th>{t("合约 / 交易日")}</th>
                          <th>{t("提交时间")}</th>
                          <th>{t("状态")}</th>
                          <th>{t("进度")}</th>
                          <th>{t("操作")}</th>
                        </tr>
                      </thead>
                      <tbody>
                        {[...research.tasks]
                          .filter(task =>
                            mode === "backtest"
                              ? task.kind === "backtest"
                              : task.kind === "backtest" ||
                                task.kind === "factor" ||
                                task.kind === "daily_factor",
                          )
                          .reverse()
                          .map(task => (
                            <tr key={task.id}>
                              <td>
                                <strong>{task.instrument}</strong>
                                <span className="subtle">
                                  {task.kind === "daily_factor"
                                    ? t("日线因子")
                                    : task.kind === "factor"
                                      ? t("动量因子")
                                      : task.trading_day}
                                </span>
                                <details>
                                  <summary>{t("详情")}</summary>
                                  <code>{task.id}</code>
                                  <p>{t("执行次数：{count}", { count: task.attempt })}</p>
                                  <p>
                                    {t("最近更新")} ·{" "}
                                    {new Date(task.updated_at_ms).toLocaleString(getLocale())}
                                  </p>
                                  {task.error && <p>{task.error}</p>}
                                </details>
                              </td>
                              <td>
                                <time dateTime={new Date(task.submitted_at_ms).toISOString()}>
                                  {new Date(task.submitted_at_ms).toLocaleString(getLocale())}
                                </time>
                              </td>
                              <td>
                                {t(states[task.state] ?? task.state)}
                                {!research.online && (
                                  <span className="subtle">{t("最后确认状态")}</span>
                                )}
                              </td>
                              <td>
                                {task.completed} / {task.total}
                              </td>
                              <td>
                                {task.kind === "backtest" && task.state !== "succeeded" && (
                                  <button
                                    disabled={busy}
                                    onClick={() => {
                                      setMode("backtest");
                                      setSelectedTask(task.id);
                                      setView("progress");
                                    }}
                                  >
                                    {t("查看进度")}
                                  </button>
                                )}
                                {task.state === "succeeded" && (
                                  <button
                                    disabled={busy || !research.online}
                                    onClick={() => {
                                      setMode(
                                        task.kind === "daily_factor"
                                          ? "daily_factor"
                                          : task.kind === "factor"
                                            ? "factor"
                                            : "backtest",
                                      );
                                      if (task.kind === "backtest") void showResult(task.id);
                                      else void run("research.result", { id: task.id });
                                    }}
                                  >
                                    {t("查看结果")}
                                  </button>
                                )}
                                {["queued", "running"].includes(task.state) && (
                                  <button
                                    disabled={busy || !research.online}
                                    onClick={() =>
                                      void run("research.action", { id: task.id, action: "cancel" })
                                    }
                                  >
                                    {t("取消")}
                                  </button>
                                )}
                                {["failed", "cancelled", "interrupted"].includes(task.state) && (
                                  <button
                                    disabled={busy || !research.online}
                                    onClick={() =>
                                      void run("research.action", { id: task.id, action: "retry" })
                                    }
                                  >
                                    {t("重新运行")}
                                  </button>
                                )}
                              </td>
                            </tr>
                          ))}
                      </tbody>
                    </table>
                  </div>
                )}
              </section>
            )}
            {mode !== "backtest" && snapshot?.research_result?.kind === "daily_factor" && (
              <DailyFactorResults
                key={snapshot.research_result.id}
                evidence={snapshot.research_result}
              />
            )}
            {mode !== "backtest" && factorResult && <FactorResults evidence={factorResult} />}
            {mode === "backtest" && view === "result" && result && result.id === selectedTask && (
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
                <svg
                  className="research-equity"
                  viewBox="0 0 640 160"
                  role="img"
                  aria-label={t("权益曲线")}
                >
                  <polyline points={points} fill="none" stroke="var(--accent)" strokeWidth="2" />
                </svg>
                <div className="research-range">
                  <span>{timestamp(result.result.equity[0]?.timestamp_ns ?? null)}</span>
                  <span>{timestamp(result.result.equity.at(-1)?.timestamp_ns ?? null)}</span>
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
    </section>
  );
}
