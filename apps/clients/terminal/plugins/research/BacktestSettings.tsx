import type {
  FixedWeightStrategyDefinition,
  HistoricalMargins,
  PerformanceAssumptions,
  PositionSides,
  SettlementMargin,
  StrategyDefinition,
  TaskOwnerStatus,
} from "../../src/bridge/client";
import { strategyRule, strategySides, strategySize, type TerminalContext } from "../contract";
import { SettlementMarginDetails, SettlementMarginEditor } from "./SettlementMargins";
import { t } from "./shared";

// The parts of a backtest's settings and its run summary. The page owns the
// drafts; each part shows and edits the ones it is given.

export type Comparison = "holdout" | "rolling";
export type Rolling = { training: string; validation: string };
export type PerformanceDraft = { annual: string; zeroRiskFree: boolean };
export type AccountParameters = {
  deposit: string;
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: string;
};
type Datasets = NonNullable<TerminalContext["snapshot"]>["datasets"];

// The performance assumptions a draft states. Choosing among candidates by
// Sharpe ratio needs the annual periods and an explicit zero risk-free rate.
export function performanceOf(draft: PerformanceDraft, candidates: number) {
  const annualPeriods = draft.annual.trim() === "" ? null : Number(draft.annual);
  const validAnnualPeriods =
    annualPeriods === null ||
    (Number.isFinite(annualPeriods) && annualPeriods > 0 && annualPeriods <= 366);
  const performanceReady =
    validAnnualPeriods && (candidates < 2 || (annualPeriods !== null && draft.zeroRiskFree));
  const performanceAssumptions: PerformanceAssumptions | undefined =
    validAnnualPeriods && (annualPeriods !== null || draft.zeroRiskFree)
      ? {
          annual_periods: annualPeriods,
          risk_free: draft.zeroRiskFree ? "explicit_zero" : "unconfigured",
        }
      : undefined;
  return { annualPeriods, validAnnualPeriods, performanceReady, performanceAssumptions };
}

// The paper account's deposit and risk limits.
export function AccountLimits({
  parameters,
  setParameters,
}: {
  parameters: AccountParameters;
  setParameters: (value: AccountParameters) => void;
}) {
  return (
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
  );
}

// How several candidates are compared, and what the candidates amount to.
export function CandidateComparison({
  candidates,
  comparison,
  setComparison,
  holdoutFrom,
  setHoldoutFrom,
  rolling,
  setRolling,
  portfolioIncomplete,
}: {
  candidates: StrategyDefinition[];
  comparison: Comparison;
  setComparison: (value: Comparison) => void;
  holdoutFrom: string;
  setHoldoutFrom: (value: string) => void;
  rolling: Rolling;
  setRolling: (value: Rolling) => void;
  portfolioIncomplete: boolean;
}) {
  return (
    <>
      {candidates.length > 1 && (
        <div className="research-fields">
          <label>
            {t("比较方式")}
            <select
              aria-label={t("比较方式")}
              value={comparison}
              onChange={event => setComparison(event.target.value as "holdout" | "rolling")}
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
                  onChange={event => setRolling({ ...rolling, [key]: event.target.value })}
                />
              </label>
            ))
          )}
        </div>
      )}
      <p className="subtle" role="status">
        {candidates.length === 0
          ? t(
              portfolioIncomplete
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
    </>
  );
}

// A downloaded historical margin snapshot, or margins entered by hand.
export function HistoricalMarginChoice({
  busy,
  supportsSettlementMargins,
  settlementMargins,
  setSettlementMargins,
  historicalMargins,
  marginChoice,
  setMarginChoice,
  invalidSettlementScope,
  previewMargins,
}: {
  busy: boolean;
  supportsSettlementMargins: boolean;
  settlementMargins: SettlementMargin[];
  setSettlementMargins: (value: SettlementMargin[]) => void;
  historicalMargins: HistoricalMargins | undefined;
  marginChoice: { selection: string; value: HistoricalMargins } | null;
  setMarginChoice: (value: null) => void;
  invalidSettlementScope: boolean;
  previewMargins: () => Promise<void>;
}) {
  return (
    <>
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
    </>
  );
}

export function PerformanceAssumptionFields({
  performanceDraft,
  setPerformanceDraft,
  validAnnualPeriods,
  candidates,
  performanceReady,
}: {
  performanceDraft: PerformanceDraft;
  setPerformanceDraft: (value: PerformanceDraft) => void;
  validAnnualPeriods: boolean;
  candidates: StrategyDefinition[];
  performanceReady: boolean;
}) {
  return (
    <fieldset>
      <legend>{t("绩效计算方法假设")}</legend>
      <p>
        {t(
          "默认按每年 242 个交易日和零无风险收益计算波动率与夏普，可以修改；年期数不从样本跨度推断，清空时保留缺项原因。比较或走步按夏普选优，须配置年期数并明确零无风险假设。",
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
  );
}

// What the backtest will run with, shown before it is submitted.
export function BacktestSummary({
  datasets,
  candidates,
  comparison,
  holdoutFrom,
  rolling,
  sides,
  fixedPortfolio,
  parameters,
  annualPeriods,
  performanceDraft,
  taskService,
  settlementMargins,
  historicalMargins,
}: {
  datasets: Datasets;
  candidates: StrategyDefinition[];
  comparison: Comparison;
  holdoutFrom: string;
  rolling: Rolling;
  sides: PositionSides;
  fixedPortfolio: FixedWeightStrategyDefinition | null;
  parameters: AccountParameters;
  annualPeriods: number | null;
  performanceDraft: PerformanceDraft;
  taskService: TaskOwnerStatus | null | undefined;
  settlementMargins: SettlementMargin[];
  historicalMargins: HistoricalMargins | undefined;
}) {
  return (
    <>
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
              : t("{n} 个候选，滚动验证：训练 {training} 个交易日，每轮验证 {validation} 个", {
                  n: candidates.length,
                  ...rolling,
                })}{" "}
          {candidates.length > 0 && <>· {strategySize(candidates[0])} </>}·{" "}
          {strategySides(candidates[0]?.sides ?? sides)}
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
          {parameters.max_gross_quantity} · {t("在途委托数上限")} {parameters.max_working_orders}
        </dd>
        <dt>{t("绩效计算方法假设")}</dt>
        <dd>
          {t("年期数A（日结算）")}: {annualPeriods ?? t("未配置")} ·{" "}
          {performanceDraft.zeroRiskFree ? t("明确零无风险") : t("无风险收益未配置")}
        </dd>
        <dt>{t("运行位置")}</dt>
        <dd>{taskService?.remote ? `${taskService.host} · ${taskService.service}` : t("本机")}</dd>
      </dl>
      <SettlementMarginDetails events={settlementMargins} historical={historicalMargins} />
    </>
  );
}

export function MatchingNotes() {
  return (
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
  );
}
