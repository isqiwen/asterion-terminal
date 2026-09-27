import { getLocale, translate } from "../contract";
import { timestamp, type ResearchResult } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.research", key);
type Evidence = Extract<ResearchResult, { kind: "backtest" | "factor" }>;
export function ExperimentDetails({ evidence }: { evidence: Evidence }) {
  const experiment = evidence.experiment;
  const contract =
    evidence.kind === "backtest"
      ? evidence.experiment.paper.contract
      : evidence.experiment.contract;
  const rows: [string, string | number][] = [
    ["合约", `${contract.venue} · ${contract.symbol}`],
    ["品种代码", contract.product],
    ["交割月份", contract.delivery_month],
    ["合约乘数", contract.multiplier],
    ["价格步长", contract.price_increment],
    ["数量步长", contract.quantity_increment],
    ["币种", contract.currency],
    ["输入成交", experiment.data.count],
  ];
  if (evidence.kind === "backtest") {
    const { paper, sma } = evidence.experiment;
    rows.push(
      ["快均线", sma.fast],
      ["慢均线", sma.slow],
      ["目标手数", sma.quantity],
      ["初始资金", paper.deposit],
      ["每手保证金", paper.costs.margin_per_lot],
      ["开仓手续费", paper.costs.open_fee],
      ["平今手续费", paper.costs.close_today_fee],
      ["平昨手续费", paper.costs.close_yesterday_fee],
      ["单笔数量上限", paper.risk.max_order_quantity],
      ["总持仓量上限", paper.risk.max_gross_quantity],
      ["在途委托数上限", paper.risk.max_working_orders],
    );
  } else {
    const { lookbacks, horizon, evaluation } = evidence.experiment;
    rows.push(
      ["候选回看笔数", lookbacks.join(", ")],
      ["未来收益笔数", horizon],
      [
        "评价方式",
        t(
          evaluation.mode === "walk_forward"
            ? "滚动验证"
            : evaluation.mode === "holdout"
              ? "时间留出评价"
              : "全样本评价",
        ),
      ],
    );
    if (evaluation.mode === "walk_forward")
      rows.push(
        ["训练成交笔数", evaluation.training_events],
        ["每轮验证笔数", evaluation.validation_events],
      );
    if (evaluation.mode === "holdout") rows.push(["前段成交笔数", evaluation.split_index]);
  }
  return (
    <details className="research-experiment">
      <summary>{t("实验参数")}</summary>
      <p className="subtle">{t("任务提交时的配置")}</p>
      <dl className="experiment-fields">
        {rows.map(([label, value]) => (
          <div key={label}>
            <dt>{t(label)}</dt>
            <dd>{value}</dd>
          </div>
        ))}
      </dl>
      {evidence.kind === "backtest" &&
        evidence.experiment.days.map(day => (
          <div className="experiment-day" key={day.trading_day}>
            <h4>
              {t("交易日")} · {day.trading_day}
            </h4>
            <dl className="experiment-fields">
              {[
                ["时段来源", day.schedule_source],
                ["结算价", day.settlement_price],
                ["结算价来源", day.settlement_source],
              ].map(([label, value]) => (
                <div key={label}>
                  <dt>{t(label)}</dt>
                  <dd>{value}</dd>
                </div>
              ))}
            </dl>
            <dl className="experiment-fields">
              {day.sessions.map((session, index) => (
                <div key={session.begin_ns}>
                  <dt>
                    {t("交易时段")} {index + 1}
                  </dt>
                  <dd>
                    {timestamp(session.begin_ns)} – {timestamp(session.end_ns)}
                  </dd>
                </div>
              ))}
            </dl>
          </div>
        ))}
      {evidence.kind === "backtest" && evidence.experiment.calendar_publication && (
        <details>
          <summary>{t("结算表版本")}</summary>
          <p>{evidence.experiment.calendar_publication.source_name}</p>
          <p>
            {t("发布标识")} <code>{evidence.experiment.calendar_publication.id}</code>
          </p>
          <p>
            {t("内容版本")}{" "}
            <code>{evidence.experiment.calendar_publication.calendar.revision}</code>
          </p>
        </details>
      )}
      <p>
        {t("输入时间范围（北京时间）")}: {timestamp(experiment.data.first_timestamp_ns)} –{" "}
        {timestamp(experiment.data.last_timestamp_ns)}
      </p>
      <p>
        {t("提交时间")}: {new Date(evidence.task.submitted_at_ms).toLocaleString(getLocale())} ·{" "}
        {t("输入版本")}: {experiment.version}
      </p>
      <p>
        {t("数据版本")} <code>{experiment.dataset_revision}</code>
      </p>
      <p>
        {t("结果摘要")} <code>{evidence.task.result_digest}</code>
      </p>
    </details>
  );
}
