import { useState } from "react";
import type { PortfolioBudgetReview, TaskResult } from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { t } from "./shared";
export function PortfolioBudget({
  result,
  query,
  busy,
}: {
  result: Pick<
    Extract<TaskResult, { kind: "backtest" }>,
    "id" | "input_sha256" | "task" | "portfolio"
  >;
} & Pick<TerminalContext, "query" | "busy">) {
  const [gross, setGross] = useState(""),
    [margin, setMargin] = useState("");
  const [review, setReview] = useState<PortfolioBudgetReview | null>(null),
    [pending, setPending] = useState(false),
    [error, setError] = useState<DisplayError>("");
  const [pathPage, setPathPage] = useState(0);
  const facts = result.portfolio;
  const points = facts.settlement_path.points;
  const lastPath = Math.max(0, Math.ceil(points.length / 24) - 1),
    currentPath = Math.min(pathPage, lastPath);
  const stateLabel = (state: string) =>
    t(
      state === "unset"
        ? "未设置"
        : state === "unknown"
          ? "未知（缺少结算占用观测）"
          : state === "exceeded"
            ? "复核超限（非执行拦截）"
            : "复核范围内（非执行许可）",
    );
  async function run() {
    setPending(true);
    setReview(null);
    setError("");
    try {
      const reply = await query("backtest.budget.review", {
        id: result.id,
        input_sha256: result.input_sha256,
        result_digest: result.task.result_digest,
        limits: { gross_notional: gross.trim() || null, margin: margin.trim() || null },
      });
      if (!reply.portfolio_budget_review) throw new Error(t("预算复核未返回确认"));
      setReview(reply.portfolio_budget_review);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <details>
      <summary>{t("组合敞口与预算复核")}</summary>
      <p>
        {t(
          "展示保存期末账户和已记录结算点，不覆盖盘中峰值或缺失观察。名义金额和保证金限额仅本次只读复核，不保存、不改原实验、不拦单，也不是风险平价或组合优化。",
        )}
      </p>
      <p className="subtle">
        {t(
          "实际期末总名义占比来自取整后持仓，不是策略目标权重。不同合约的代数净名义仅为记账汇总，不代表风险相互抵销。保存末价可能来自不同交易日。历史保证金、乘数和规格适用性仍未证实，不用当前值填补。",
        )}
      </p>
      <div className="research-table">
        <table aria-label={t("逐合约期末敞口")}>
          <thead>
            <tr>
              {[
                "合约",
                "净手数",
                "总手数",
                "手数步长",
                "保存末价",
                "固定乘数",
                "净名义金额",
                "总名义金额",
                "实际期末总名义占比",
                "模型保证金",
                "证据",
              ].map(key => (
                <th key={key}>{t(key)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {facts.contracts.map(row => (
              <tr key={`${row.venue}/${row.symbol}`}>
                <td>
                  {row.venue}/{row.symbol}
                </td>
                <td>{row.net_quantity}</td>
                <td>{row.gross_quantity}</td>
                <td>{row.quantity_increment}</td>
                <td>{row.mark === "0" ? t("未知") : row.mark}</td>
                <td>{row.multiplier}</td>
                <td>{row.net_notional}</td>
                <td>{row.gross_notional}</td>
                <td>
                  {row.gross_notional_share === null
                    ? t("未定义")
                    : `${(row.gross_notional_share * 100).toFixed(2)}%`}
                </td>
                <td>{row.margin}</td>
                <td>
                  {t(row.quantities_on_grid ? "持仓与成交符合手数步长" : "持仓或成交不符手数步长")}
                  <br />
                  {row.cost_source} · {row.cost_effective_from}
                  <br />
                  {t("每手保证金")} {row.margin_per_lot} · {t("名义保证金率")} {row.margin_rate}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p>
        {t("期末代数净名义")} {facts.net_notional} · {t("期末总名义")} {facts.gross_notional} ·{" "}
        {t("期末模型保证金")} {facts.model_margin} · {t("记录保证金")} {facts.recorded_margin} ·{" "}
        {t("保证金差额（记录减模型）")} {facts.margin_difference}
      </p>
      <p>
        {t("余额")} {facts.balance} · {t("权益")} {facts.equity} · {t("可用资金")} {facts.available}{" "}
        · {t("冻结资金")} {facts.frozen}
      </p>
      <p className="subtle">
        {t(
          "名义敞口不是现金。模型保证金是资金占用，不再次扣减权益；可用资金沿原账户口径 min（余额，权益）减保证金与冻结。保证金按保存的各批次持仓与原固定成本日程计算，保留逐批次舍入。",
        )}
      </p>
      <p>
        {t("原固定风控参数")} · {t("单笔手数上限")} {facts.risk.max_order_quantity} ·{" "}
        {t("总手数上限")} {facts.risk.max_gross_quantity} · {t("在途委托数上限")}{" "}
        {facts.risk.max_working_orders} · {t("期末手数余量")} {facts.risk.final_lot_headroom}
      </p>
      <p className="subtle">
        {t(
          "内置限额风控在执行前检查单笔手数、已持仓加待开仓手数、在途委托数；账户另检查资金可用性。这里展示的期末余量不证明全程未超限，实际执行还取决于原记录的风控实现。",
        )}
      </p>
      <p className="subtle">
        {t("风控实现摘要")} {result.task.risk_artifact || t("未知")}
      </p>
      <label>
        {t("总名义限额（期末与结算点复核）")}
        <input
          value={gross}
          disabled={pending}
          onChange={event => {
            setGross(event.target.value);
            setReview(null);
            setError("");
          }}
        />
      </label>
      <label>
        {t("模型保证金限额（期末与结算点复核）")}
        <input
          value={margin}
          disabled={pending}
          onChange={event => {
            setMargin(event.target.value);
            setReview(null);
            setError("");
          }}
        />
      </label>
      <p className="subtle">
        {t(
          "至少明确填写一个非负金额；留空表示未设置，不按零预算或默认风险偏好处理。修改输入会清除上次复核结果。",
        )}
      </p>
      <button
        disabled={busy || pending || (!gross.trim() && !margin.trim())}
        onClick={() => void run()}
      >
        {t("复核记录点预算")}
      </button>
      <ErrorNotice error={error} />
      <details>
        <summary>{t("逐结算日占用与预算轨迹")}</summary>
        <p className="subtle">
          {t(
            "仅观察原结果已记录的结算点，不是盘中峰值。跨品种代数净额为零不代表风险对冲或风险为零。旧结果缺少总手数、批次保证金或可用资金观测时保持未知，不以净手数或当前参数重算，也不插补日期。",
          )}
        </p>
        {review && (
          <div>
            {(["gross_notional", "margin"] as const).map(key => {
              const summary = review.settlement_summary[key];
              return (
                <p key={key}>
                  {t(key === "gross_notional" ? "总名义结算点复核" : "模型保证金结算点复核")} ·{" "}
                  {t("已检查点")} {summary.checked_points} ·{" "}
                  <strong>
                    {t("已知超限点")} {summary.exceeded_points ?? t("未设置")}
                  </strong>{" "}
                  · {t("未知点")} {summary.unknown_points}
                </p>
              );
            })}
          </div>
        )}
        <div className="research-table">
          <table aria-label={t("逐结算日预算轨迹")}>
            <thead>
              <tr>
                {[
                  "交易日",
                  "代数净名义",
                  "总名义金额",
                  "模型保证金",
                  "余额",
                  "权益",
                  "可用资金",
                  "冻结资金",
                  "总名义余量",
                  "保证金余量",
                  "总名义状态",
                  "保证金状态",
                ].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {points.slice(currentPath * 24, (currentPath + 1) * 24).map((point, index) => {
                const checks = review?.settlement_checks[currentPath * 24 + index]?.checks;
                return (
                  <tr key={point.day}>
                    {[
                      point.day,
                      point.net_notional,
                      point.gross_notional,
                      point.model_margin,
                      point.balance,
                      point.equity,
                      point.available,
                      point.frozen,
                    ].map((value, i) => (
                      <td key={i}>{value ?? t("未知")}</td>
                    ))}
                    <td>
                      {checks
                        ? (checks.gross_notional.headroom ??
                          t(checks.gross_notional.state === "unset" ? "未设置" : "未知"))
                        : t("未复核")}
                    </td>
                    <td>
                      {checks
                        ? (checks.margin.headroom ??
                          t(checks.margin.state === "unset" ? "未设置" : "未知"))
                        : t("未复核")}
                    </td>
                    <td>{checks ? stateLabel(checks.gross_notional.state) : t("未复核")}</td>
                    <td>{checks ? stateLabel(checks.margin.state) : t("未复核")}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        {lastPath > 0 && (
          <div>
            <button disabled={currentPath === 0} onClick={() => setPathPage(currentPath - 1)}>
              {t("上一页")}
            </button>{" "}
            {currentPath + 1}/{lastPath + 1}{" "}
            <button
              disabled={currentPath === lastPath}
              onClick={() => setPathPage(currentPath + 1)}
            >
              {t("下一页")}
            </button>
          </div>
        )}
      </details>
      {review && (
        <div className="research-table">
          <table aria-label={t("期末预算复核结果")}>
            <thead>
              <tr>
                {["项目", "已用", "显式限额", "余量（限额减已用）", "状态"].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {(["gross_notional", "margin"] as const).map(key => {
                const check = review.checks[key];
                return (
                  <tr key={key}>
                    <td>{t(key === "gross_notional" ? "期末总名义" : "期末模型保证金")}</td>
                    <td>{check.used}</td>
                    <td>{check.limit ?? t("未设置")}</td>
                    <td>{check.headroom ?? t("未知")}</td>
                    <td>
                      {t(
                        check.state === "unset"
                          ? "未设置"
                          : check.state === "exceeded"
                            ? "复核超限（非执行拦截）"
                            : "复核范围内（非执行许可）",
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          <p className="subtle">
            {review.source.task_id} · input {review.source.input_sha256} · result{" "}
            {review.source.result_digest}
          </p>
        </div>
      )}
    </details>
  );
}
