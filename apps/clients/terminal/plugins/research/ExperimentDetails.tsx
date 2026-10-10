import { HistoryAvailability } from "./HistoryAvailability";
import { CostScheduleDetails, getLocale, strategyRows, translate } from "../contract";
import {
  timestamp,
  type CostVersion,
  type DatasetEvidence,
  type ExperimentData,
  type TaskResult,
} from "../../src/bridge/client";
import { factorWords } from "./shared";
const t = (key: string) => translate("asterion.terminal.research", key);
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
  return {
    name: `${contract.venue} · ${contract.symbol}`,
    rows,
    schedule,
    range: [data.first_timestamp_ns, data.last_timestamp_ns],
  };
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
  // Already in the reader's language: the strategy names its own fields.
  let strategyFacts: Rows = [];
  let contracts;
  if (evidence.kind === "backtest") {
    const { paper, strategies, data } = evidence.experiment;
    const strategy = strategies[evidence.result.selected];
    contracts = paper.contracts.map((item, index) =>
      contractRows(item.dataset, data[index], item.cost_schedule),
    );
    strategyFacts = strategyRows(strategy);
    rows.push(
      ["初始资金", paper.deposit],
      ["单笔数量上限", paper.risk.max_order_quantity],
      ["总持仓量上限", paper.risk.max_gross_quantity],
      ["在途委托数上限", paper.risk.max_working_orders],
    );
    if (strategies.length > 1)
      rows.push(["候选策略数", strategies.length], ["留出起始日", evidence.experiment.holdout_day]);
  } else {
    const { series, lookbacks, horizon, evaluation } = evidence.experiment;
    const words = factorWords[series[0].kind];
    contracts = series.map(input =>
      input.kind === "bars"
        ? contractRows(input.dataset, input.data)
        : {
            name: input.data.contract_id,
            rows: [
              ["交易日范围", `${input.data.first_day} – ${input.data.last_day}`],
              [words.input, input.data.count],
              ["数据源", input.data.source],
              ["来源数据版本", input.data.source_dataset_id],
              ["来源摘要", input.data.manifest_sha256],
            ] as Rows,
          },
    );
    rows.push(
      [words.candidates, lookbacks.join(", ")],
      [words.horizon, horizon],
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
        [words.training, evaluation.training_events],
        [words.validation, evaluation.validation_events],
      );
    if (evaluation.mode === "holdout") rows.push([words.split, evaluation.split_index]);
  }
  let versions;
  if (evidence.kind === "backtest")
    versions = evidence.experiment.paper.contracts.flatMap(item => item.dataset.history_evidence);
  else
    versions = evidence.experiment.series.flatMap(input =>
      input.kind === "bars" ? input.dataset.history_evidence : [input.data.history_evidence],
    );
  return (
    <>
      <HistoryAvailability
        versions={[...new Map(versions.map(value => [value.dataset_id, value])).values()]}
      />
      <details className="research-experiment">
        <summary>{t("实验参数")}</summary>
        <p className="subtle">{t("任务提交时的配置")}</p>
        {strategyFacts.length > 0 && (
          <dl className="experiment-fields">
            {strategyFacts.map(([label, value]) => (
              <div key={label}>
                <dt>{label}</dt>
                <dd>{value}</dd>
              </div>
            ))}
          </dl>
        )}
        <Fields rows={rows} />
        {contracts.map(contract => (
          <section key={contract.name} aria-label={contract.name}>
            <h4>{contract.name}</h4>
            <Fields rows={contract.rows} />
            {"schedule" in contract && contract.schedule && (
              <CostScheduleDetails versions={contract.schedule} />
            )}
            {"range" in contract && (
              <p>
                {t("输入时间范围（北京时间）")}: {timestamp(contract.range[0])} –{" "}
                {timestamp(contract.range[1])}
              </p>
            )}
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
