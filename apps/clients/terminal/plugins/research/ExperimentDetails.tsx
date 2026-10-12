import { HistoryAvailability } from "./HistoryAvailability";
import { CostScheduleDetails, getLocale, strategyRows, strategyRule, translate } from "../contract";
import {
  timestamp,
  type CostVersion,
  type DatasetEvidence,
  type ExperimentData,
  type FactorSeriesEvidence,
  type TaskResult,
} from "../../src/bridge/client";
import { factorNames, factorWords } from "./shared";
const t = (key: string) => translate("asterion.terminal.research", key);
type Evidence = Extract<TaskResult, { kind: "backtest" | "factor" }>;
type Rows = [string, string | number][];
// One block of the details: a contract or series with what was read of it.
type Block = { name: string; rows: Rows; schedule?: CostVersion[]; range?: string[] };
// One contract of the experiment: its terms, input range and, for backtests, costs.
function contractRows(
  dataset: DatasetEvidence,
  data: ExperimentData,
  schedule?: CostVersion[],
  slippage?: number,
): Block {
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
    // Daily bars have no archive version: they are the daily table's own.
    ["K 线数据版本", dataset.source_dataset_ids.join(" · ") || t("取自全市场日线表")],
    // Settlement prices and trading days are the market-wide daily table's.
    ["数据表快照", dataset.daily.snapshot],
    ["日线来源", dataset.daily.daily_source],
    ["交易日历来源", dataset.daily.calendar_source],
  ];
  if (slippage !== undefined) rows.push(["滑点（跳）", slippage]);
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
// A product read as its dominant series: the months it was read from and the
// day each took over, with the factor its prices were scaled by.
function dominantRows(input: Extract<FactorSeriesEvidence, { kind: "dominant" }>): Block {
  const first = input.months[0];
  const last = input.months[input.months.length - 1];
  const contract = first.dataset.contract;
  const versions = [
    ...new Set(input.months.flatMap(month => month.dataset.source_dataset_ids)),
  ].join(" · ");
  const rows: Rows = [
    ["品种代码", contract.product],
    ["月份合约", input.months.map(month => month.dataset.contract.symbol).join(" · ")],
    [
      "换月",
      input.rolls
        .map(
          roll =>
            `${roll.trading_day} ${input.months[roll.contract].dataset.contract.symbol} ×${roll.factor}`,
        )
        .join(" · "),
    ],
    ["合约乘数", contract.multiplier],
    ["价格步长", contract.price_increment],
    ["K 线周期（分钟）", first.data.interval_minutes],
    ["输入 K 线", input.count],
    ["有期限结构的交易日", input.terms],
    ["交易日范围", `${first.data.first_day} – ${last.data.last_day}`],
    ["数据源", first.data.source],
    ["K 线数据版本", versions || t("取自全市场日线表")],
    // Settlements and open interest of every month are the daily table's.
    ["数据表快照", first.dataset.daily.snapshot],
    ["日线来源", first.dataset.daily.daily_source],
    ["交易日历来源", first.dataset.daily.calendar_source],
  ];
  return {
    name: `${contract.venue} · ${contract.product} · ${t("主力连续")}`,
    rows,
    range: [first.data.first_timestamp_ns, last.data.last_timestamp_ns],
  };
}
// An instrument of the market-wide daily table: the months it was read from,
// when each took over, and the snapshot they were read under.
function tableRows(input: Extract<FactorSeriesEvidence, { kind: "table" }>): Block {
  const data = input.data;
  const read = data.months.filter(month => month.first_day && month.last_day);
  const rows: Rows = [
    ["月份合约", data.months.map(month => month.contract).join(" · ")],
    ["输入日线", data.count],
    ["交易日范围", read.length ? `${read[0].first_day} – ${read[read.length - 1].last_day}` : "—"],
    ["数据源", data.daily_source],
    ["交易日历来源", data.calendar_source],
    ["数据表快照", data.snapshot],
  ];
  if (data.rolls.length)
    rows.splice(
      1,
      0,
      [
        "换月",
        data.rolls
          .map(roll => `${roll.trading_day} ${data.months[roll.contract].contract} ×${roll.factor}`)
          .join(" · "),
      ],
      ["有期限结构的交易日", data.terms],
    );
  return {
    name: data.rolls.length ? `${data.instrument} · ${t("主力连续")}` : data.instrument,
    rows,
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
  let contracts: Block[];
  if (evidence.kind === "backtest") {
    const { paper, strategies, data } = evidence.experiment;
    const rolling = evidence.experiment.walk_forward;
    contracts = paper.contracts.map((item, index) =>
      contractRows(item.dataset, data[index], item.cost_schedule, item.slippage_ticks),
    );
    // A rolling comparison follows several strategies in turn: it lists them
    // all below instead of the fields of one.
    if (!rolling) strategyFacts = strategyRows(strategies[evidence.result.selected]);
    rows.push(
      ["初始资金", paper.deposit],
      ["单笔数量上限", paper.risk.max_order_quantity],
      ["总持仓量上限", paper.risk.max_gross_quantity],
      ["在途委托数上限", paper.risk.max_working_orders],
    );
    if (rolling)
      rows.push(
        ["候选策略", strategies.map(strategyRule).join(" · ")],
        ["训练交易日数", rolling.training_days],
        ["每轮验证交易日数", rolling.validation_days],
      );
    else if (strategies.length > 1)
      rows.push(["候选策略数", strategies.length], ["留出起始日", evidence.experiment.holdout_day]);
  } else {
    const { series, lookbacks, horizon, evaluation } = evidence.experiment;
    const words =
      factorWords[series[0].kind === "daily" || series[0].kind === "table" ? "daily" : "bars"];
    contracts = series.map(input =>
      input.kind === "bars"
        ? contractRows(input.dataset, input.data)
        : input.kind === "dominant"
          ? dominantRows(input)
          : input.kind === "table"
            ? tableRows(input)
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
      ["因子", t(factorNames[evidence.experiment.factor])],
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
  // A snapshot of the daily table stands for everything read under it.
  const snapshot = (id: string, acquired_at_ns: string) => ({
    dataset_id: id,
    acquired_at_ns,
    source_availability: "unknown" as const,
  });
  // A dataset's bars come from archive versions, its settlement prices from
  // the daily table.
  const dataset = (item: DatasetEvidence) => [
    ...item.history_evidence,
    snapshot(item.daily.snapshot, item.daily_acquired_at_ns),
  ];
  let versions;
  if (evidence.kind === "backtest")
    versions = evidence.experiment.paper.contracts.flatMap(item => dataset(item.dataset));
  else
    versions = evidence.experiment.series.flatMap(input =>
      input.kind === "bars"
        ? dataset(input.dataset)
        : input.kind === "dominant"
          ? input.months.flatMap(month => dataset(month.dataset))
          : input.kind === "table"
            ? [snapshot(input.data.snapshot, input.data.acquired_at_ns)]
            : [input.data.history_evidence],
    );
  // Once each; of a snapshot read several times, the most recent partition.
  const acquired = new Map<string, (typeof versions)[number]>();
  for (const version of versions) {
    const known = acquired.get(version.dataset_id);
    if (!known || BigInt(version.acquired_at_ns) > BigInt(known.acquired_at_ns))
      acquired.set(version.dataset_id, version);
  }
  return (
    <>
      <HistoryAvailability versions={[...acquired.values()]} />
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
            {contract.schedule && <CostScheduleDetails versions={contract.schedule} />}
            {contract.range && (
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
