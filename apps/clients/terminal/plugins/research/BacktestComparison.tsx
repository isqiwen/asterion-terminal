import { taskOwner } from "../../src/bridge/client";
import { SettlementMarginDetails } from "./SettlementMargins";
import { ComparisonExport } from "./ComparisonExport";
import { PortfolioReport } from "./PortfolioReport";
import { useEffect, useRef, useState } from "react";
import type { PerformanceFigures, TaskResult } from "../../src/bridge/client";
import {
  CostScheduleDetails,
  ErrorNotice,
  asDisplayError,
  strategyRows,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { namespace, t } from "./shared";
export type BacktestEvidence = Extract<TaskResult, { kind: "backtest" }>;
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
export function backtestComparisonReasons(
  base: BacktestEvidence,
  value: BacktestEvidence,
): string[] {
  const a = base.experiment,
    b = value.experiment;
  const reasons: string[] = [];
  if (a.dataset_revision !== b.dataset_revision) reasons.push("固定数据内容不同");
  const datasets = (v: BacktestEvidence) => v.experiment.paper.contracts.map(c => c.dataset);
  if (
    !same(
      datasets(base).map(d => d.daily.snapshot),
      datasets(value).map(d => d.daily.snapshot),
    )
  )
    reasons.push("固定快照不同");
  if (!same(a.data, b.data)) reasons.push("行情范围、频率或来源不同");
  if (
    !same(
      datasets(base).map(d => [d.contract, d.price_increment_changes]),
      datasets(value).map(d => [d.contract, d.price_increment_changes]),
    )
  )
    reasons.push("合约规格或变动价位不同");
  const costs = (v: BacktestEvidence) => v.experiment.paper.contracts.map(c => c.cost_schedule);
  if (
    !same(costs(base), costs(value)) ||
    !same(a.settlement_margins ?? [], b.settlement_margins ?? []) ||
    !same(a.historical_margins ?? null, b.historical_margins ?? null)
  )
    reasons.push("费用或保证金日程不同");
  if (!same(a.paper.risk, b.paper.risk) || base.task.risk_artifact !== value.task.risk_artifact)
    reasons.push("风险限额或实现不同");
  if (!base.task.risk_artifact || !value.task.risk_artifact) reasons.push("风险实现摘要缺失");
  if (
    !same(
      a.paper.contracts.map(c => c.slippage_ticks),
      b.paper.contracts.map(c => c.slippage_ticks),
    )
  )
    reasons.push("滑点不同");
  if (!same(a.series ?? [], b.series ?? [])) reasons.push("换月日程或调整因子不同");
  if (a.paper.deposit !== b.paper.deposit) reasons.push("初始资金不同");
  if (
    a.holdout_day !== b.holdout_day ||
    !same(a.walk_forward, b.walk_forward) ||
    !same(
      base.result.folds.map(f => f.first_day),
      value.result.folds.map(f => f.first_day),
    )
  )
    reasons.push("训练或验证边界不同");
  if (
    !same(
      base.result.settlements.map(d => d.trading_day),
      value.result.settlements.map(d => d.trading_day),
    )
  )
    reasons.push("实际结算交易日不同");
  return reasons;
}
function validationRanges(v: BacktestEvidence): string {
  const days = v.result.settlements.map(d => d.trading_day);
  if (v.experiment.walk_forward)
    return v.result.folds
      .map(
        (f, i) =>
          `${f.first_day} – ${days.filter(day => !v.result.folds[i + 1] || day < v.result.folds[i + 1].first_day).at(-1) ?? "—"}`,
      )
      .join("; ");
  if (v.experiment.holdout_day)
    return `${t("前段")}: ${days[0] ?? "—"} – ${days.filter(day => day < v.experiment.holdout_day).at(-1) ?? "—"}; ${t("留出段")}: ${v.experiment.holdout_day} – ${days.at(-1) ?? "—"}`;
  return t("不适用");
}
const percent = (v: number | null) => (v === null ? t("未定义") : `${(v * 100).toFixed(4)}%`);
const ratio = (v: number | null) => (v === null ? t("未定义") : v.toFixed(4));
export function BacktestComparison({
  snapshot,
  busy,
  query,
}: Pick<TerminalContext, "snapshot" | "busy" | "query">) {
  const [selected, setSelected] = useState<string[]>([]);
  const [loaded, setLoaded] = useState<BacktestEvidence[]>([]);
  const [failures, setFailures] = useState<{ id: string; error: DisplayError }[]>([]);
  const [loading, setLoading] = useState(false);
  const generation = useRef({ value: 0 });
  const connection = snapshot?.research_service?.connection_id;
  useEffect(() => {
    const epoch = generation.current;
    epoch.value++;
    setSelected([]);
    setLoaded([]);
    setFailures([]);
    setLoading(false);
    return () => {
      epoch.value++;
    };
  }, [connection]);
  const tasks =
    snapshot?.research_service?.tasks.filter(
      task => task.kind === "backtest" && task.state === "succeeded",
    ) ?? [];
  const table = useRef<HTMLTableElement>(null);
  const base = loaded[0];
  async function compare() {
    const current = generation.current.value;
    setLoading(true);
    setLoaded([]);
    setFailures([]);
    const replies = await Promise.allSettled(
      selected.map(async id => {
        const reply = await query("task.result.read", {
          owner: taskOwner(snapshot?.research_service, "research"),
          id,
        });
        const value = reply.task_result;
        if (value?.kind !== "backtest") throw new Error(t("所选任务没有回测结果"));
        return value;
      }),
    );
    if (current !== generation.current.value) return;
    setLoaded(replies.flatMap(reply => (reply.status === "fulfilled" ? [reply.value] : [])));
    setFailures(
      replies.flatMap((reply, index) =>
        reply.status === "rejected"
          ? [{ id: selected[index], error: asDisplayError(reply.reason) }]
          : [],
      ),
    );
    setLoading(false);
  }
  const row = (label: string, values: string[]) => (
    <tr key={label}>
      <th scope="row">{t(label)}</th>
      {values.map((value, i) => (
        <td
          key={loaded[i].id}
          data-value-state={
            label === "期末权益" || label === "累计手续费" ? "defined" : "display_only"
          }
        >
          {value}
          {i > 0 && value !== values[0] && <strong> · {t("不同")}</strong>}
        </td>
      ))}
    </tr>
  );
  const figures = (label: string, select: (v: BacktestEvidence) => PerformanceFigures | null) => (
    <tr key={label}>
      <th scope="row">{t(label)}</th>
      {loaded.map(value => {
        const p = select(value);
        return (
          <td key={value.id} data-value-state={p ? "metric_group" : "not_applicable"}>
            {p ? (
              <dl>
                {[
                  ["交易日数", String(p.trading_days), "defined"],
                  ["总收益率", percent(p.total_return), "defined"],
                  ["最大回撤", percent(p.max_drawdown), "defined"],
                  ["夏普比率", ratio(p.sharpe), p.sharpe === null ? "undefined" : "defined"],
                  [
                    "年化收益率",
                    percent(p.annual_return),
                    p.annual_return === null ? "undefined" : "defined",
                  ],
                  [
                    "年化波动率",
                    percent(p.annual_volatility),
                    p.annual_volatility === null ? "undefined" : "defined",
                  ],
                  ["卡玛比率", ratio(p.calmar), p.calmar === null ? "undefined" : "defined"],
                  ["盈利交易日占比", percent(p.winning_days), "defined"],
                ].map(([name, v, state]) => (
                  <div key={name}>
                    <dt>{t(name)}</dt>
                    <dd data-value-state={state}>{v}</dd>
                  </div>
                ))}
              </dl>
            ) : (
              t("不适用")
            )}
          </td>
        );
      })}
    </tr>
  );
  return (
    <section className="research-result" aria-label={t("回测实验比较")}>
      <h3>{t("回测实验比较")}</h3>
      <p className="subtle">
        {t(
          "选择 2–3 个已完成实验，只读取固定结果。首列为对照；口径不同仅独立列示，不排序、不合并，也不重新选参。",
        )}
      </p>
      <fieldset disabled={busy || loading}>
        <legend>{t("选择比较实验")}</legend>
        {tasks.map(task => (
          <label key={task.id}>
            <input
              type="checkbox"
              checked={selected.includes(task.id)}
              disabled={selected.length === 3 && !selected.includes(task.id)}
              onChange={event => {
                setLoaded([]);
                setFailures([]);
                setSelected(
                  event.target.checked
                    ? [...selected, task.id]
                    : selected.filter(id => id !== task.id),
                );
              }}
            />
            {task.id} · {task.instrument}
            {task.metadata?.name ? ` · ${task.metadata.name}` : ""}
          </label>
        ))}
        <button disabled={selected.length < 2} onClick={() => void compare()}>
          {t("读取并比较")}
        </button>
      </fieldset>
      <p>
        {t("已选实验")}:{" "}
        {selected.map(id => (
          <code key={id}>{id} </code>
        ))}
      </p>
      <button
        disabled={loading || selected.length === 0}
        onClick={() => {
          setSelected([]);
          setLoaded([]);
          setFailures([]);
        }}
      >
        {t("清空比较选择")}
      </button>
      {failures.length > 0 && <p>{t("部分结果不可读，以下仅列示已读取结果。")}</p>}
      {loading && <p role="status">{t("正在读取固定结果")}</p>}
      {failures.map(value => (
        <div key={value.id}>
          <code>{value.id}</code>
          <ErrorNotice error={value.error} namespace={namespace} />
        </div>
      ))}
      {base && (
        <>
          {failures.length === 0 && loaded.length === selected.length && (
            <ComparisonExport
              key={loaded.map(v => v.id).join(":")}
              table={table}
              evidence={loaded}
              reasons={loaded.map(v => backtestComparisonReasons(base, v))}
              busy={busy || loading}
              query={query}
            />
          )}
          <p>
            {t(
              "全程包含训练交易日，不等于样本外成绩；留出段承接前段权益，滚动样本外从首轮验证开始。年化指标不足 20 个交易日时未定义；不存在的评价段标为不适用。只列原结果，不按留出成绩重新选优。",
            )}
          </p>
          <div className="research-table">
            <table ref={table} aria-label={t("回测实验对照")}>
              <thead>
                <tr>
                  <th>{t("项目")}</th>
                  {loaded.map(v => (
                    <th key={v.id}>
                      <code>{v.id}</code>
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {row(
                  "实验名称",
                  loaded.map(v => v.task.metadata?.name || "—"),
                )}
                {row(
                  "实验备注",
                  loaded.map(v => v.task.metadata?.note || "—"),
                )}
                {row(
                  "比较口径",
                  loaded.map(v => {
                    const reasons = backtestComparisonReasons(base, v);
                    return reasons.length
                      ? t("仅独立列示") + ": " + reasons.map(r => t(r)).join("；")
                      : t("同口径对照");
                  }),
                )}
                {row(
                  "策略候选与参数",
                  loaded.map(v =>
                    v.experiment.strategies
                      .map(
                        (s, i) =>
                          `${i + 1}: ${strategyRows(s)
                            .map(([k, x]) => `${k}: ${x}`)
                            .join(" / ")}`,
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "选中候选",
                  loaded.map(v =>
                    v.experiment.walk_forward
                      ? v.result.folds
                          .map(
                            f =>
                              `${f.first_day}: ${f.selected === null ? t("未选中") : f.selected + 1}`,
                          )
                          .join("; ")
                      : String(v.result.selected + 1),
                  ),
                )}
                {row(
                  "评价模式",
                  loaded.map(v =>
                    t(
                      v.experiment.walk_forward
                        ? "滚动验证"
                        : v.experiment.holdout_day
                          ? "时间留出评价"
                          : "全样本评价",
                    ),
                  ),
                )}
                {row(
                  "训练与验证边界",
                  loaded.map(v =>
                    v.experiment.walk_forward
                      ? `${t("训练天数")}: ${v.experiment.walk_forward.training_days}; ${t("验证天数")}: ${v.experiment.walk_forward.validation_days}; ${v.result.folds.map(f => f.first_day).join(", ")}`
                      : v.experiment.holdout_day
                        ? `${t("留出起始日")}: ${v.experiment.holdout_day}`
                        : t("不适用"),
                  ),
                )}
                {row("实际验证区间", loaded.map(validationRanges))}
                {row(
                  "行情范围与频率",
                  loaded.map(v =>
                    v.experiment.data
                      .map(
                        (d, i) =>
                          `${v.experiment.paper.contracts[i].dataset.contract.symbol}: ${d.first_day} – ${d.last_day}; ${d.interval_minutes === 0 ? t("日线") : d.interval_minutes + " min"}; ${d.count}`,
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "实际结算区间",
                  loaded.map(
                    v =>
                      `${v.result.settlements[0]?.trading_day ?? "—"} – ${v.result.settlements.at(-1)?.trading_day ?? "—"} (${v.result.settlements.length})`,
                  ),
                )}
                {row(
                  "初始资金",
                  loaded.map(v => v.experiment.paper.deposit),
                )}
                {row(
                  "风险限额",
                  loaded.map(
                    v =>
                      `${t("单笔数量上限")}: ${v.experiment.paper.risk.max_order_quantity}; ${t("总持仓量上限")}: ${v.experiment.paper.risk.max_gross_quantity}; ${t("在途委托数上限")}: ${v.experiment.paper.risk.max_working_orders}`,
                  ),
                )}
                {row(
                  "风险实现摘要",
                  loaded.map(v => v.task.risk_artifact || t("未知")),
                )}
                {row(
                  "滑点（跳）",
                  loaded.map(v =>
                    v.experiment.paper.contracts
                      .map(c => `${c.dataset.contract.symbol}: ${c.slippage_ticks}`)
                      .join("; "),
                  ),
                )}
                <tr>
                  <th scope="row">{t("逐日费用与保证金")}</th>
                  {loaded.map((v, i) => (
                    <td key={v.id}>
                      {i > 0 &&
                        !same(
                          v.experiment.paper.contracts.map(c => c.cost_schedule),
                          base.experiment.paper.contracts.map(c => c.cost_schedule),
                        ) && <strong>{t("不同")}</strong>}
                      <SettlementMarginDetails
                        events={v.experiment.settlement_margins}
                        historical={v.experiment.historical_margins}
                      />
                      {v.experiment.paper.contracts.map(c => (
                        <div key={c.dataset.contract.symbol}>
                          <code>{c.dataset.contract.symbol}</code>
                          <CostScheduleDetails versions={c.cost_schedule} />
                        </div>
                      ))}
                    </td>
                  ))}
                </tr>
                {row(
                  "合约规格与变动价位",
                  loaded.map(v =>
                    v.experiment.paper.contracts
                      .map(
                        c =>
                          `${c.dataset.contract.symbol}: ${c.dataset.contract.currency}; ${t("合约乘数")}: ${c.dataset.contract.multiplier}; ${t("最小变动价位")}: ${c.dataset.contract.price_increment}; ${t("数量步长")}: ${c.dataset.contract.quantity_increment}; ${c.dataset.price_increment_changes.map(x => `${x.trading_day}: ${x.value}`).join(", ")}`,
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "换月日程",
                  loaded.map(v =>
                    v.experiment.series?.length
                      ? v.experiment.series
                          .map(s =>
                            s
                              .map(
                                roll =>
                                  `${roll.trading_day}: ${v.experiment.paper.contracts[roll.contract].dataset.contract.symbol} × ${roll.factor}`,
                              )
                              .join(" / "),
                          )
                          .join("; ")
                      : t("不适用"),
                  ),
                )}
                {row(
                  "固定快照",
                  loaded.map(v =>
                    v.experiment.paper.contracts
                      .map(c => `${c.dataset.contract.symbol}: ${c.dataset.daily.snapshot}`)
                      .join("; "),
                  ),
                )}
                {row(
                  "数据内容摘要",
                  loaded.map(v => v.experiment.dataset_revision),
                )}
                {row(
                  "行情版本与来源",
                  loaded.map(v =>
                    v.experiment.paper.contracts
                      .map(
                        c =>
                          `${c.dataset.contract.symbol}: ${c.dataset.revision}; ${c.dataset.source}; ${c.dataset.source_dataset_ids.join(", ")}; ${c.dataset.daily.daily_source} / ${c.dataset.daily.calendar_source} / ${c.dataset.daily.contracts_source}`,
                      )
                      .join("; "),
                  ),
                )}
                {row(
                  "固定输入摘要",
                  loaded.map(v => v.input_sha256),
                )}
                {row(
                  "结果摘要",
                  loaded.map(v => v.task.result_digest),
                )}
                {figures("全程绩效", v => v.performance)}
                {figures("前段绩效", v => v.performance.development)}
                {figures("留出段绩效", v => v.performance.holdout)}
                {figures("滚动样本外绩效", v => v.performance.out_of_sample)}
                {row(
                  "期末权益",
                  loaded.map(v => v.result.account.equity),
                )}
                {row(
                  "累计手续费",
                  loaded.map(v => v.result.account.fees),
                )}
              </tbody>
            </table>
          </div>
        </>
      )}
      {loaded.length >= 2 && failures.length === 0 && (
        <PortfolioReport
          key={loaded.map(value => `${value.id}:${value.task.result_digest}`).join("|")}
          sources={loaded.map(value => ({ id: value.id, result_digest: value.task.result_digest }))}
          busy={busy || loading}
          query={query}
        />
      )}
    </section>
  );
}
