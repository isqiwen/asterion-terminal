import { HistoryAvailability } from "./HistoryAvailability";
import { CostScheduleDetails, getLocale, translate } from "../contract";
import {
  timestamp,
  type CostVersion,
  type DatasetEvidence,
  type ExperimentData,
  type TaskResult,
} from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.backtest-factor", key);
type Evidence = Extract<TaskResult, { kind: "backtest" | "factor" }>;
type Rows = [string, string | number][];
// One contract of the experiment: its terms, input range and, for backtests, costs.
function contractRows(dataset: DatasetEvidence, data: ExperimentData, schedule?: CostVersion[]) {
  const contract = dataset.contract;
  const rows: Rows = [
    ["品种代码", contract.product],
    ["交割月份", contract.delivery_month],
    ["合约乘数", contract.multiplier],
    ["价格步长", contract.price_increment],
    ["数量步长", contract.quantity_increment],
    ["币种", contract.currency],
    ["K 线周期（分钟）", data.interval_minutes],
    ["输入 K 线", data.count],
    ["交易日范围", `${data.first_day} – ${data.last_day}`],
    ["数据源", data.source],
    ["K 线数据版本", dataset.source_dataset_ids.join(" · ")],
    ["结算价数据版本", dataset.settlement_dataset_ids.join(" · ")],
  ];
  const costs = schedule?.filter(row => row.effective_from <= data.first_day).at(-1)?.values;
  if (costs)
    rows.push(
      ["每手保证金", costs.margin_per_lot],
      ["开仓手续费", costs.open_fee],
      ["平今手续费", costs.close_today_fee],
      ["平昨手续费", costs.close_yesterday_fee],
      ["保证金率", costs.margin_rate],
      ["开仓费率", costs.open_fee_rate],
      ["平今费率", costs.close_today_fee_rate],
      ["平昨费率", costs.close_yesterday_fee_rate],
    );
  return { name: `${contract.venue} · ${contract.symbol}`, rows, data, schedule };
}
function Fields({ rows }: { rows: Rows }) {
  return (
    <dl className="experiment-fields">
      {rows.map(([label, value]) => (
        <div key={label}>
          <dt>{t(label)}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}
export function ExperimentDetails({ evidence }: { evidence: Evidence }) {
  const experiment = evidence.experiment;
  const rows: Rows = [];
  let contracts;
  if (evidence.kind === "backtest") {
    const { paper, sma, data } = evidence.experiment;
    contracts = paper.contracts.map((item, index) =>
      contractRows(item.dataset, data[index], item.cost_schedule),
    );
    rows.push(
      ["快均线", sma.fast],
      ["慢均线", sma.slow],
      ["目标手数", sma.quantity],
      ["初始资金", paper.deposit],
      ["单笔数量上限", paper.risk.max_order_quantity],
      ["总持仓量上限", paper.risk.max_gross_quantity],
      ["在途委托数上限", paper.risk.max_working_orders],
    );
  } else {
    const { dataset, data, lookbacks, horizon, evaluation } = evidence.experiment;
    contracts = [contractRows(dataset, data)];
    rows.push(
      ["候选回看 K 线数", lookbacks.join(", ")],
      ["未来收益 K 线数", horizon],
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
        ["训练 K 线数", evaluation.training_events],
        ["每轮验证 K 线数", evaluation.validation_events],
      );
    if (evaluation.mode === "holdout") rows.push(["前段 K 线数", evaluation.split_index]);
  }
  const versions =
    evidence.kind === "backtest"
      ? evidence.experiment.paper.contracts.flatMap(item => item.dataset.history_evidence)
      : evidence.experiment.dataset.history_evidence;
  return (
    <>
      <HistoryAvailability
        versions={[...new Map(versions.map(value => [value.dataset_id, value])).values()]}
      />
      <details className="backtest-factor-experiment">
        <summary>{t("实验参数")}</summary>
        <p className="subtle">{t("任务提交时的配置")}</p>
        <Fields rows={rows} />
        {contracts.map(contract => (
          <section key={contract.name} aria-label={contract.name}>
            <h4>{contract.name}</h4>
            <Fields rows={contract.rows} />
            {contract.schedule && <CostScheduleDetails versions={contract.schedule} />}
            <p>
              {t("输入时间范围（北京时间）")}: {timestamp(contract.data.first_timestamp_ns)} –{" "}
              {timestamp(contract.data.last_timestamp_ns)}
            </p>
          </section>
        ))}
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
    </>
  );
}
