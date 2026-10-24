import { useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type TaskOwnerRef,
  type TaskResult,
  type ResourceObject,
  type EvaluationScope,
  type EvaluationStressScenario,
  type EvaluationLimitation,
} from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { namespace, t } from "./shared";
import { percent, ratio } from "./Performance";
import { EvaluationExport } from "./EvaluationExport";
import { DailyDrawdownView } from "./DailyDrawdownView";
import { ExposureView } from "./ExposureView";
import { LedgerView } from "./LedgerView";
import { ContributionsView } from "./ContributionsView";
import { evaluationMetricReasons as reasons } from "./EvaluationMetricReasons";

type Report = Extract<TaskResult, { kind: "performance_evaluation" }>;
export type EvaluationSource = {
  object: ResourceObject;
  owner: TaskOwnerRef;
  program: string;
  scopes: EvaluationScope[];
  retired: boolean;
};
const scopes: Record<EvaluationScope, string> = {
  full_account: "全程账户",
  development: "前段",
  holdout: "留出段",
  out_of_sample: "样本外",
};
const statuses = {
  queued: "排队中",
  running: "运行中",
  cancel_requested: "正在取消",
  publishing: "正在发布",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};

const limitations: Record<EvaluationLimitation, string> = {
  CASH_FLOWS_NOT_EVALUATED: "未评价真实现金流调整",
  BENCHMARK_NOT_EVALUATED: "未评价基准与相对表现",
  RISK_AND_STRESS_NOT_EVALUATED: "未评价风险压力情景",
  RISK_AND_STRESS_INCOMPLETE: "仅已请求的风险事实或静态情景；完整归因、尾部与动态压力尚未评价",
  INDEPENDENCE_NOT_ESTABLISHED: "未证明独立验证资格",
  HISTORICAL_AVAILABILITY_NOT_VERIFIED: "未核实真实历史可得性",
  HUMAN_CONCLUSION_NOT_RECORDED: "未记录人工支持或不支持结论",
};

type Draft = {
  question: string;
  scope: EvaluationScope;
  annual: string;
  zero: boolean;
  daily: boolean;
  ledger: boolean;
  contributions: boolean;
  exposure: boolean;
  stress: boolean;
  stressPurpose: string;
  stressPrice: string;
  stressRate: string;
  stressFunds: boolean;
};
export function EvaluationReports({
  snapshot,
  query,
  trade,
  busy,
  workspaceParams,
  source,
}: Pick<TerminalContext, "snapshot" | "query" | "trade" | "busy" | "workspaceParams"> & {
  source: EvaluationSource | null;
}) {
  const service = snapshot?.research_service;
  const destination = JSON.stringify([
    service?.connection_id,
    service?.node_id,
    service?.owner_node_id,
    service?.service,
  ]);
  const sourceKey = JSON.stringify([destination, source?.object]);
  const [draft, setDraft] = useWorkspaceDraft<Draft>(`evaluation-draft:${sourceKey}`, {
    question: "",
    scope: "full_account",
    annual: "",
    zero: false,
    daily: false,
    ledger: false,
    contributions: false,
    exposure: false,
    stress: false,
    stressPurpose: "",
    stressPrice: "",
    stressRate: "",
    stressFunds: false,
  });
  const annual = draft.annual.trim() === "" ? null : Number(draft.annual);
  const method = {
    annual_periods: annual,
    risk_free: draft.zero ? ("explicit_zero" as const) : ("unconfigured" as const),
  };
  const stress: EvaluationStressScenario | null = draft.stress
    ? {
        purpose: draft.stressPurpose.trim(),
        price: draft.stressPrice.trim(),
        margin_rate: draft.stressRate.trim(),
        funds_model: "paper_balance_equity_minimum",
      }
    : null;
  const [operation, setOperation] = useWorkspaceRequestId(
    `evaluation-submit:${sourceKey}`,
    JSON.stringify({
      source: source?.object,
      question: draft.question.trim(),
      scope: draft.scope,
      method,
      include_daily_drawdown: draft.daily,
      include_exposure: draft.exposure,
      ...(draft.ledger ? { include_ledger: true } : {}),
      ...(draft.contributions ? { include_contributions: true } : {}),
      static_stress: stress,
    }),
  );
  useWorkspaceUnsaved(
    `evaluation-draft:${sourceKey}`,
    Boolean(source && draft.question.trim() && !operation),
  );
  const routeId =
    workspaceParams?.kind === "performance_evaluation" &&
    workspaceParams.connection_id === service?.connection_id &&
    workspaceParams.service === service?.service &&
    workspaceParams.owner_node_id === service?.owner_node_id
      ? (workspaceParams.id ?? "")
      : "";
  const [reportId, setReportId] = useWorkspaceDraft(`evaluation-report:${destination}`, "");
  const [saved, setSaved] = useState<{ report: Report; owner: TaskOwnerRef } | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [pending, setPending] = useState(false);
  const generation = useRef(0);
  useEffect(() => {
    generation.current++;
    setSaved(null);
    setPending(false);
    setError("");
    if (routeId) setReportId(routeId);
    return () => {
      // Invalidates owned asynchronous replies after navigation.
      // eslint-disable-next-line react-hooks/exhaustive-deps
      generation.current++;
    };
  }, [destination, routeId, setReportId]);
  const matches = (owner: TaskOwnerRef) =>
    owner.kind === "research" &&
    owner.connection_id === service?.connection_id &&
    owner.node_id === service?.node_id &&
    owner.service === service?.service;
  const currentSource =
    source && matches(source.owner) && source.object.node === service?.owner_node_id
      ? source
      : null;
  const blocked = busy || pending || !service?.online;
  const valid =
    currentSource &&
    currentSource.program &&
    !currentSource.retired &&
    currentSource.object.node &&
    draft.question.trim() &&
    currentSource.scopes.includes(draft.scope) &&
    (!draft.ledger || draft.scope === "full_account") &&
    (!draft.contributions || draft.scope === "full_account") &&
    (annual === null || (Number.isFinite(annual) && annual > 0 && annual <= 366)) &&
    (!stress ||
      (draft.exposure &&
        draft.stressFunds &&
        stress.purpose &&
        /^-?\d+(\.\d{1,8})?$/.test(stress.price) &&
        /^\d+(\.\d{1,8})?$/.test(stress.margin_rate)));
  const reports = service?.tasks.filter(task => task.kind === "performance_evaluation") ?? [];
  const selected = reports.find(task => task.id === reportId);
  const report = saved && matches(saved.owner) ? saved.report : null;
  async function submit() {
    if (blocked || !valid || !currentSource) return;
    const current = ++generation.current;
    const id = operation ?? `evaluation-${crypto.randomUUID()}`;
    setOperation(id);
    setPending(true);
    setError("");
    try {
      await trade("evaluation.submit", {
        id,
        owner: currentSource.owner,
        source: currentSource.object,
        question: draft.question.trim(),
        scope: draft.scope,
        method,
        include_daily_drawdown: draft.daily,
        include_exposure: draft.exposure,
        ...(draft.ledger ? { include_ledger: true } : {}),
        ...(draft.contributions ? { include_contributions: true } : {}),
        static_stress: stress,
      });
      if (current !== generation.current) return;
      setReportId(id);
      setSaved(null);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setPending(false);
    }
  }
  async function read() {
    if (blocked || !reportId || !service) return;
    const current = ++generation.current;
    const owner = taskOwner(service, "research");
    setPending(true);
    setError("");
    setSaved(null);
    try {
      const reply = await query("task.result.read", { owner, id: reportId });
      if (current !== generation.current) return;
      if (reply.task_result?.kind !== "performance_evaluation" || reply.task_result.id !== reportId)
        throw new Error(t("所选任务没有固定绩效评价结果"));
      setSaved({ report: reply.task_result, owner });
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setPending(false);
    }
  }
  return (
    <section aria-label={t("固定绩效评价")}>
      <h3>{t("固定绩效评价")}</h3>
      <p className="subtle">
        {t(
          "固定原账户事实与本次方法生成正式评价；不重跑原回测，不覆盖原发布评分，不自动产生人工结论或交易资格。",
        )}
      </p>
      {!source && <p>{t("先读取原回测，核对其固定身份后再创建评价。")}</p>}
      {source && !source.object.node && (
        <p role="status">{t("Research稳定业务身份不可用，不能固定来源")}</p>
      )}
      {currentSource && (
        <>
          {currentSource.retired && <p role="status">{t("原来源已退役，不能创建新评价。")}</p>}
          <p>
            {t("固定来源")}：{currentSource.object.id} · <code>{currentSource.object.version}</code>
          </p>
          <p>
            {t("原生产程序")}：<code>{currentSource.program || t("未记录")}</code>
          </p>
          {!currentSource.program && (
            <p role="status">{t("原生产程序缺证据，不能创建固定评价。")}</p>
          )}
        </>
      )}
      <fieldset disabled={blocked || !currentSource}>
        <label>
          {t("评价问题与用途")}
          <input
            maxLength={4096}
            value={draft.question}
            onChange={event => setDraft({ ...draft, question: event.target.value })}
          />
        </label>
        <label>
          {t("评价范围")}
          <select
            value={draft.scope}
            onChange={event => setDraft({ ...draft, scope: event.target.value as EvaluationScope })}
          >
            {(currentSource?.scopes ?? ["full_account" as const]).map(scope => (
              <option key={scope} value={scope}>
                {t(scopes[scope])}
              </option>
            ))}
          </select>
        </label>
        <label>
          {t("年期数A（日结算）")}
          <input
            type="number"
            min="0"
            max="366"
            step="any"
            value={draft.annual}
            onChange={event => setDraft({ ...draft, annual: event.target.value })}
          />
        </label>
        <label>
          <input
            type="checkbox"
            checked={draft.zero}
            onChange={event => setDraft({ ...draft, zero: event.target.checked })}
          />
          {t("明确采用零无风险年收益假设")}
        </label>
        <label>
          <input
            type="checkbox"
            checked={Boolean(draft.ledger)}
            onChange={event => setDraft({ ...draft, ledger: event.target.checked })}
          />
          {t("保存原账本归因桥接（仅完整账户、单合约日线）")}
        </label>
        {draft.ledger && (
          <p>{t("仅使用固定原成交和日结；不支持范围将明确拒绝，不代表完整多维归因或独立验证。")}</p>
        )}
        <label>
          <input
            type="checkbox"
            checked={draft.contributions}
            onChange={e => setDraft({ ...draft, contributions: e.target.checked })}
          />
          {t("保存原合约贡献（仅完整账户、全部日线）")}
        </label>
        {draft.contributions && (
          <p>{t("按原合约与原成交日期解释金额；缺证据拒绝，不分摊账户本金或未解释差额。")}</p>
        )}
        <label>
          <input
            type="checkbox"
            checked={draft.daily}
            onChange={event => setDraft({ ...draft, daily: event.target.checked })}
          />
          {t("保存日采样损失路径（不代表盘中最大风险）")}
        </label>
        <label>
          <input
            type="checkbox"
            checked={draft.exposure}
            onChange={event => setDraft({ ...draft, exposure: event.target.checked })}
          />
          {t("保存本范围末日敞口和资金观察（非盘中峰值）")}
        </label>
        <label>
          <input
            type="checkbox"
            checked={draft.stress}
            onChange={event => setDraft({ ...draft, stress: event.target.checked })}
          />
          {t("保存显式静态价格与保证金压力")}
        </label>
        {draft.stress && (
          <fieldset>
            <legend>{t("固定持仓静态情景")}</legend>
            <p>
              {t(
                "仅一个原合约且原总手数和资金完整；先选择原末日敞口。无成交、调仓、费用变化或自动补资。",
              )}
            </p>
            <label>
              {t("压力用途与范围")}
              <input
                maxLength={4096}
                value={draft.stressPurpose}
                onChange={event => setDraft({ ...draft, stressPurpose: event.target.value })}
              />
            </label>
            <label>
              {t("情景价格（原价格单位）")}
              <input
                value={draft.stressPrice}
                onChange={event => setDraft({ ...draft, stressPrice: event.target.value })}
              />
            </label>
            <label>
              {t("假设保证金率（比例，0.4表示40%）")}
              <input
                value={draft.stressRate}
                onChange={event => setDraft({ ...draft, stressRate: event.target.value })}
              />
            </label>
            <label>
              <input
                type="checkbox"
                checked={draft.stressFunds}
                onChange={event => setDraft({ ...draft, stressFunds: event.target.checked })}
              />
              {t("明确采用原纸面资金模型：余额与权益取小，减保证金及原冻结")}
            </label>
          </fieldset>
        )}
        <p>{t("未选择年期数或无风险假设时，分别固定不可计算原因，不从原实验或样本跨度补值。")}</p>
        <button disabled={!valid} onClick={() => void submit()}>
          {t(operation ? "重试同一评价请求" : "计算固定绩效评价")}
        </button>
      </fieldset>
      {operation && (
        <p role="status">
          {t("接纳不等于发布；结果不确定时保留同一请求ID，先核对原任务。")}:{" "}
          <code>{operation}</code>
        </p>
      )}
      <fieldset disabled={blocked}>
        <label>
          {t("已保存绩效评价")}
          <select
            value={reportId}
            onChange={event => {
              setReportId(event.target.value);
              setSaved(null);
            }}
          >
            <option value="">{t("选择固定绩效评价")}</option>
            {reportId && !selected && <option value={reportId}>{reportId}</option>}
            {reports.map(task => (
              <option key={task.id} value={task.id}>
                {task.id} · {t(task.retired ? "已退役" : statuses[task.state])}
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={!reportId || Boolean(selected && selected.state !== "succeeded")}
          onClick={() => void read()}
        >
          {t("读取固定绩效评价")}
        </button>
      </fieldset>
      <ErrorNotice error={error} namespace={namespace} />
      {report && <EvaluationReportView report={report} />}
      {report && saved && (
        <EvaluationExport
          report={report}
          owner={saved.owner}
          query={query}
          busy={busy || pending}
        />
      )}
    </section>
  );
}

export function EvaluationReportView({ report }: { report: Report }) {
  return (
    <article aria-label={t("固定评价报告")}>
      <h4>{report.experiment.request.question}</h4>
      <p>
        {report.id} · <code>{report.task.result_digest}</code>
      </p>
      <dl>
        <dt>{t("固定来源")}</dt>
        <dd>
          {report.experiment.request.source.id} ·{" "}
          <code>{report.experiment.request.source.version}</code>
        </dd>
        <dt>{t("原生产程序")}</dt>
        <dd>
          <code>{report.experiment.source.program_artifact}</code>
        </dd>
        <dt>{t("评价执行程序")}</dt>
        <dd>
          <code>{report.task.program_artifact ?? t("未记录")}</code>
        </dd>
        <dt>{t("评价范围")}</dt>
        <dd>
          {t(scopes[report.experiment.request.scope])} · {report.result.first_day} —{" "}
          {report.result.last_day}
        </dd>
        <dt>{t("原观测起点（UTC纳秒）")}</dt>
        <dd>{report.result.begin_ns}</dd>
        <dt>{t("原观测终点（UTC纳秒）")}</dt>
        <dd>{report.result.end_ns}</dd>
        <dt>{t("有效完整收益周期")}</dt>
        <dd>{report.result.complete_return_periods}</dd>
        <dt>{t("年期数A（日结算）")}</dt>
        <dd>{report.experiment.request.method.annual_periods ?? t("未配置")}</dd>
        <dt>{t("无风险收益假设")}</dt>
        <dd>
          {report.experiment.request.method.risk_free === "explicit_zero"
            ? t("明确零无风险")
            : t("无风险收益未配置")}
        </dd>
        <dt>{t("初期权益")}</dt>
        <dd>
          {report.result.opening_equity} {report.result.currency}
        </dd>
        <dt>{t("期末权益")}</dt>
        <dd>
          {report.result.closing_equity} {report.result.currency}
        </dd>
        <dt>{t("实际记账费用")}</dt>
        <dd>
          {report.result.fees} {report.result.currency}
        </dd>
        <dt>{t("净损益")}</dt>
        <dd>
          {report.result.net_profit} {report.result.currency}
        </dd>
      </dl>
      <div className="research-metrics">
        {(
          [
            ["total_return", "总收益率", true],
            ["annual_return", "年化收益率", true],
            ["annual_volatility", "年化波动率", true],
            ["sharpe", "夏普比率", false],
            ["max_drawdown", "最大回撤", true],
            ["calmar", "卡玛比率", false],
            ["winning_days", "盈利交易日占比", true],
          ] as const
        ).map(([key, label, percentage]) => {
          const metric = report.result[key];
          return (
            <div key={key}>
              <span>{t(label)}</span>
              <strong>{percentage ? percent(metric.value) : ratio(metric.value)}</strong>
              {metric.state !== "AVAILABLE" && <small>{t(reasons[metric.state])}</small>}
            </div>
          );
        })}
      </div>
      <p>
        {t(
          report.result.opening_is_full_period
            ? "起点为原结算，首个完整周期参与统计。"
            : "起点到首日结算为开端短周期，保留金额损益但不混入完整周期波动率和夏普。",
        )}
      </p>
      <p>{t("正式评价数值由Research固定发布，页面只格式化显示；修改草稿不会重算或替换本报告。")}</p>
      {report.result.short_calendar_span && <p>{t("观测不足一年；年化仅为方法缩放值。")}</p>}
      <p>{t("年化收益使用实际秒数和365.2425天；波动率与夏普按sqrt(A)缩放，未作自相关调整。")}</p>
      {report.result.daily_drawdown && (
        <DailyDrawdownView
          key={report.task.result_digest}
          path={report.result.daily_drawdown}
          currency={report.result.currency}
        />
      )}
      {report.result.ledger && (
        <LedgerView
          key={report.task.result_digest}
          ledger={report.result.ledger}
          currency={report.result.currency}
        />
      )}
      {report.result.contributions && (
        <ContributionsView
          key={report.task.result_digest}
          evidence={report.result.contributions}
          currency={report.result.currency}
        />
      )}
      {report.result.exposure && (
        <ExposureView
          key={report.task.result_digest}
          slice={report.result.exposure}
          equity={report.result.closing_equity}
          currency={report.result.currency}
        />
      )}
      {report.result.static_stress && report.experiment.request.static_stress && (
        <section aria-label={t("固定静态压力结果")}>
          <h5>{t("固定持仓静态情景")}</h5>
          <p>{report.experiment.request.static_stress.purpose}</p>
          <p>
            {t("无成交、调仓、费用变化或强平成交证据；假设保证金不是市值损失，也不是现行费率。")}
          </p>
          <dl>
            <dt>{t("情景价格（原价格单位）")}</dt>
            <dd>{report.experiment.request.static_stress.price}</dd>
            <dt>{t("假设保证金率（比例，0.4表示40%）")}</dt>
            <dd>{report.experiment.request.static_stress.margin_rate}</dd>
            <dt>{t("资金模型")}</dt>
            <dd>{t("原纸面模型；余额及冻结不变，权益按市值变化")}</dd>
            {(
              [
                ["mark_change", "情景市值变化"],
                ["equity", "情景权益"],
                ["margin", "情景保证金占用"],
                ["available", "情景可用资金"],
                ["gap", "情景资金缺口"],
              ] as const
            ).map(([key, label]) => (
              <div key={key}>
                <dt>{t(label)}</dt>
                <dd>
                  {report.result.static_stress?.[key]} {report.result.currency}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      )}
      <ul aria-label={t("评价未覆盖范围")}>
        {report.result.limitations.map(limit => (
          <li key={limit}>{t(limitations[limit])}</li>
        ))}
      </ul>
    </article>
  );
}
