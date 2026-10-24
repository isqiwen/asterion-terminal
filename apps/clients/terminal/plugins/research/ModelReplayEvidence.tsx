import { useState } from "react";
import type { TaskResult } from "../../src/bridge/client";
import { t } from "./shared";
type Evidence = Extract<TaskResult, { kind: "backtest" }>;
const boundary = (raw: string) =>
  `${new Date(Number(BigInt(raw) / 1000000n)).toISOString()} · ${raw}`;
export function ModelReplayEvidence({ evidence }: { evidence: Evidence }) {
  const [page, setPage] = useState(0);
  const definition = evidence.experiment.model_replay,
    report = evidence.result.model_replay;
  if (!definition || !report) return null;
  const phases = {
    before_training: "尚未训练",
    model_pending: "模型处理中",
    outside_validation: "评价范围之外",
    feature_unavailable: "特征不可用",
    non_finite_prediction: "预测不可用",
    expired: "已到期",
    mapped: "已预测映射",
  } as const;
  const dispositions = {
    no_new_target: "无新目标",
    next_bar: "等待后续bar风控与撮合",
    outside_replay: "执行范围已结束，不提交",
  } as const;

  const rows = report.decisions.slice(page * 50, page * 50 + 50);
  return (
    <section aria-label={t("模型行情模拟证据")}>
      <h4>{t("模型行情模拟证据")}</h4>
      <p>
        {t(
          "历史可得时间未知；按明确bar收盘假设重放，训练和处理延迟之后才预测。这里的方向、成交和收益不是实盘授权或独立历史可交易证据。",
        )}
      </p>
      <p>
        {t("固定逐折映射")}: <code>{definition.mapping.object.id}</code> ·{" "}
        <code>{definition.mapping.object.version}</code>
      </p>
      <p>
        {t("模拟原折")}: {definition.fold_index + 1} · {t("目标手数")}: {definition.quantity} ·{" "}
        {t("模型处理延迟（bar）")}: {definition.processing_delay_bars}
      </p>
      <dl>
        <dt>{t("实际模拟训练边界")}</dt>
        <dd>
          {report.fit.started_index} · <code>{boundary(report.fit.started_boundary_ns)}</code>
        </dd>
        <dt>{t("模拟模型可用边界")}</dt>
        <dd>
          {report.fit.ready_index} · <code>{boundary(report.fit.ready_boundary_ns)}</code>
        </dd>
        <dt>{t("训练成员原索引")}</dt>
        <dd>{report.fit.training_members.join(", ")}</dd>
        <dt>{t("原训练拟合参数")}</dt>
        <dd>
          {report.fit.intercept} · {report.fit.coefficient}
        </dd>
        <dt>{t("实际计算耗时（纳秒）")}</dt>
        <dd>{report.fit.actual_duration_ns}</dd>
      </dl>
      {evidence.result.model_validation && (
        <p>
          {t("仅验证交易日评价")}: {evidence.result.model_validation.total_return} ·{" "}
          {evidence.result.model_validation.max_drawdown} ·{" "}
          {evidence.result.model_validation.sharpe ?? "—"}
        </p>
      )}
      <table aria-label={t("模型逐bar决策原件")}>
        <thead>
          <tr>
            {[
              "原行索引",
              "原边界UTC与纳秒",
              "模型阶段",
              "原特征",
              "原预测",
              "新目标",
              "目标状态",
            ].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(row => (
            <tr key={row.source_index}>
              <td>{row.source_index}</td>
              <td>{boundary(row.boundary_ns)}</td>
              <td>{t(phases[row.phase])}</td>
              <td>{row.feature ?? "—"}</td>
              <td>{row.prediction ?? "—"}</td>
              <td>{row.target ?? "—"}</td>
              <td>
                {t(dispositions[row.disposition])} <code>{row.intent_id}</code>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button disabled={page === 0} onClick={() => setPage(value => value - 1)}>
        {t("上一页")}
      </button>
      <button
        disabled={(page + 1) * 50 >= report.decisions.length}
        onClick={() => setPage(value => value + 1)}
      >
        {t("下一页")}
      </button>
      <table aria-label={t("模型决策与真实模拟成交关联")}>
        <thead>
          <tr>
            <th>{t("成交编号")}</th>
            <th>{t("原决策索引")}</th>
            <th>{t("原成交索引")}</th>
            <th>{t("原边界UTC与纳秒")}</th>
          </tr>
        </thead>
        <tbody>
          {report.fills.map(fill => (
            <tr key={fill.fill_id}>
              <td>{fill.fill_id}</td>
              <td>{fill.decision_source_index}</td>
              <td>{fill.source_index}</td>
              <td>{boundary(fill.boundary_ns)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  );
}

export function TargetRejectionEvidence({ evidence }: { evidence: Evidence }) {
  const reasons = {
    order_quantity: "单笔数量上限",
    gross_quantity: "总持仓量上限",
    working_orders: "在途委托数上限",
    insufficient_funds: "保证金及手续费所需可用资金不足",
  } as const;
  if (!evidence.result.target_rejections?.length) return null;
  return (
    <table aria-label={t("模拟风控拒单原件")}>
      <thead>
        <tr>
          <th>{t("原行索引")}</th>
          <th>{t("新目标")}</th>
          <th>{t("拒单原因")}</th>
        </tr>
      </thead>
      <tbody>
        {evidence.result.target_rejections.map(row => (
          <tr key={row.intent_id}>
            <td>{row.source_index}</td>
            <td>{row.target}</td>
            <td>
              {t(reasons[row.reason])} <code>{row.intent_id}</code>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
