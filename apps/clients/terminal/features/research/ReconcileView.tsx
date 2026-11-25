import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import { lab } from "../../src/bridge/lab";
import { tradingCall } from "../../src/bridge/trading";
import type { Instrument } from "../../src/generated/asterion/common_pb";
import type { ReconcileFill, ReconcileStep } from "../../src/generated/asterion/lab_pb";
import type { Bar } from "../../src/generated/asterion/strategy_pb";
import type { StrategyStep } from "../../src/generated/asterion/trading_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { type MessageValues, listSeparator, translate } from "../../src/i18n";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { defaultForm, readConditions, saveConditions, scopeOf } from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const STEP_PAGE = 500;
const BAR_PAGE = 1000;
const TRADE_PAGE = 500;
const MAX_STEPS = 10000;
const MAX_BARS_PER_INSTRUMENT = 10000;

type BarSummary = {
  matched: number;
  missing_in_live: number;
  extra_in_live: number;
  mismatched: number;
  max_close_diff: string;
  max_volume_diff: string;
  ok: boolean;
};

type DecisionSummary = {
  matched: number;
  missing_in_live: number;
  extra_in_live: number;
  mismatched: number;
  ok: boolean;
};

type FillSummary = {
  live_fill_count: number;
  backtest_fill_count: number;
  matched: number;
  live_only: number;
  backtest_only: number;
  mismatched: number;
  max_price_diff: string;
  mean_adverse_price: string | null;
  slippage_samples: number;
  mean_slippage_ticks: number | null;
  median_slippage_ticks: number | null;
  p90_slippage_ticks: number | null;
  max_slippage_ticks: number | null;
  ok: boolean;
};

type BarDiff = {
  kind: string;
  end_ms: number;
  trading_day: string;
  symbol: string;
  fields?: Record<
    string,
    { live?: string | null; backtest?: string | null; delta?: string | null }
  >;
  live_close?: string | null;
  backtest_close?: string | null;
};

type DecisionDiff = {
  kind: string;
  end_ms: number;
  sequence?: number | null;
  caused_by_bar_diff?: boolean;
  ready?: { live: boolean; backtest: boolean };
  targets?: Record<
    string,
    { mode?: string; live?: string | null; backtest?: string | null; delta?: string | null }
  >;
};

type FillDiff = {
  kind: string;
  trading_day: string;
  contract: string;
  side: string;
  offset: string;
  live_qty?: string | null;
  backtest_qty?: string | null;
  live_vwap?: string | null;
  backtest_vwap?: string | null;
  price_delta?: string | null;
  adverse_price?: string | null;
  adverse_ticks?: number | null;
};

export type ReconcileReport = {
  ok: boolean;
  metadata: {
    instance_id: string;
    run_id: string;
    package: string;
    dataset_version: string;
    period: string;
    symbols: string[];
    slippage_ticks?: number;
    window_begin_day: string;
    window_end_day: string;
    fill_window_begin: string;
  };
  summary: {
    bars: BarSummary;
    recorded_bars: BarSummary;
    decisions: DecisionSummary;
    fills: FillSummary;
  };
  bar_diffs: BarDiff[];
  recorded_bar_diffs: BarDiff[];
  decision_diffs: DecisionDiff[];
  fill_diffs: FillDiff[];
};

const instrumentKey = (inst?: Instrument) => (inst ? `${inst.exchange}/${inst.symbol}` : "");

function matchRate(item: {
  matched: number;
  missing_in_live: number;
  extra_in_live: number;
  mismatched: number;
}) {
  const total = item.matched + item.missing_in_live + item.extra_in_live + item.mismatched;
  if (total === 0) return { rate: "—", detail: "0 / 0" };
  return {
    rate: `${((item.matched / total) * 100).toFixed(2)}%`,
    detail: `${item.matched} / ${total}`,
  };
}

const ticksText = (value: number | null) =>
  value === null || !Number.isFinite(value)
    ? "—"
    : `${value > 0 ? "+" : ""}${value.toFixed(2).replace(/\.00$/, "")}`;

const sideText = (side: string) => (side === "buy" ? t("买") : side === "sell" ? t("卖") : side);

const offsetText = (offset: string) =>
  (
    ({
      open: t("开仓"),
      close: t("平仓"),
      close_today: t("平今"),
      close_yesterday: t("平昨"),
    }) as Record<string, string>
  )[offset] ?? offset;

const diffKindText = (kind: string) =>
  (
    ({
      missing_in_live: t("实盘缺失"),
      extra_in_live: t("实盘多余"),
      field_mismatch: t("字段不一致"),
      decision_mismatch: t("目标不一致"),
      live_only: t("仅实盘成交"),
      backtest_only: t("仅回测成交"),
      fill_mismatch: t("量价不一致"),
    }) as Record<string, string>
  )[kind] ?? kind;

async function fetchLiveAndReconcile(
  accountId: string,
  instanceId: string,
  runId: string,
  repository: string,
): Promise<ReconcileReport> {
  const strategies = await tradingCall(accountId, { case: "getStrategies", value: {} });
  const targetInstance = instanceId.trim() || strategies.instance?.instance?.id || "";

  const allSteps: StrategyStep[] = [];
  let configuration = strategies.instance?.instance;
  let afterSequence = 0n;
  while (allSteps.length < MAX_STEPS) {
    const page = await tradingCall(accountId, {
      case: "getStrategyRun",
      value: { id: targetInstance, afterSequence, limit: STEP_PAGE },
    });
    if (page.configuration) configuration = page.configuration;
    if (!page.steps.length) break;
    allSteps.push(...page.steps);
    const lastSeq = page.steps[page.steps.length - 1]?.input?.sequence ?? 0n;
    if (page.steps.length < STEP_PAGE || lastSeq <= afterSequence) break;
    afterSequence = lastSeq;
  }
  if (!configuration || allSteps.length === 0) {
    throw new Error(t("该账户没有可对账的策略运行记录"));
  }

  const matchedPkg = strategies.packages.find(
    pkg => pkg.name === configuration.packageName && pkg.version === configuration.packageVersion,
  );
  const resolvedRunId = runId.trim() || matchedPkg?.research?.runId || "";
  const resolvedDatasetVersion =
    configuration.warmup?.datasetVersion || matchedPkg?.research?.datasetVersion || "";

  const tracked = new Map<string, Instrument>();
  for (const inst of configuration.instruments) tracked.set(instrumentKey(inst), inst);
  for (const series of configuration.series) {
    for (const entry of series.contracts) {
      if (entry.instrument) tracked.set(instrumentKey(entry.instrument), entry.instrument);
    }
  }

  const recordedBars: Bar[] = [];
  for (const inst of tracked.values()) {
    let afterEndMs = 0n;
    let count = 0;
    while (count < MAX_BARS_PER_INSTRUMENT) {
      const page = await tradingCall(accountId, {
        case: "getRecordedBars",
        value: {
          period: configuration.period,
          instrument: inst,
          afterEndMs,
          limit: BAR_PAGE,
        },
      });
      if (!page.bars.length) break;
      recordedBars.push(...page.bars);
      count += page.bars.length;
      const lastEnd = page.bars[page.bars.length - 1]?.endMs ?? 0n;
      if (page.bars.length < BAR_PAGE || lastEnd <= afterEndMs) break;
      afterEndMs = lastEnd;
    }
  }

  const stepDays = Array.from(
    new Set(
      allSteps.flatMap(step => (step.input?.bars ?? []).map(bar => bar.tradingDay).filter(Boolean)),
    ),
  ).sort();
  const seenFills = new Set<string>();
  const liveFills: Array<Omit<ReconcileFill, "$typeName" | "$unknown">> = [];
  for (const day of stepDays) {
    let offset = 0;
    while (true) {
      const page = await tradingCall(accountId, {
        case: "getDay",
        value: { tradingDay: day, offset, limit: TRADE_PAGE, oldestFirst: true },
      });
      if (!page.trades.length) break;
      for (const trade of page.trades) {
        const instKey = instrumentKey(trade.instrument);
        if (tracked.size > 0 && !tracked.has(instKey)) continue;
        const dedup = `${trade.tradingDay}|${trade.id}|${trade.side}`;
        if (seenFills.has(dedup)) continue;
        seenFills.add(dedup);
        liveFills.push({
          tradingDay: trade.tradingDay,
          instrument: trade.instrument,
          side: trade.side,
          offset: trade.offset,
          lots: trade.lots,
          price: trade.price,
        });
      }
      offset += page.trades.length;
      if (offset >= page.total || page.trades.length < TRADE_PAGE) break;
    }
  }

  const stepsPayload: Array<Omit<ReconcileStep, "$typeName" | "$unknown">> = allSteps.map(
    (step, index) => ({
      sequence: step.input?.sequence ?? BigInt(index + 1),
      input: step.input,
      decision: step.decision,
    }),
  );

  const reply = await lab({
    case: "reconcileLive",
    value: {
      instance: configuration,
      steps: stepsPayload,
      recordedBars,
      liveFills,
      runId: resolvedRunId,
      repository,
      datasetVersion: resolvedDatasetVersion,
    },
  });
  return JSON.parse(reply.report) as ReconcileReport;
}

export function ReconcileView({
  accounts,
  repository,
  initialAccount,
  initialInstance,
  initialRun,
  autoStart,
  back,
  openRun,
}: {
  accounts: TradingConnection[];
  repository: string;
  initialAccount?: string;
  initialInstance?: string;
  initialRun?: string;
  autoStart?: boolean;
  back: () => void;
  openRun?: (id: string) => void;
}) {
  const [accountId, setAccountId] = useState(
    () => accounts.find(a => a.id === initialAccount)?.id ?? accounts[0]?.id ?? "",
  );
  const [instanceId, setInstanceId] = useState(initialInstance ?? "");
  const [runId, setRunId] = useState(initialRun ?? "");
  const [running, setRunning] = useState(false);
  const [report, setReport] = useState<ReconcileReport | null>(null);
  const [appliedTicks, setAppliedTicks] = useState<number | null>(null);
  const acted = useActionError();
  const autoStarted = useRef(false);

  const execute = useCallback(
    (acct: string, inst: string, run: string) => {
      if (!acct) return;
      setRunning(true);
      setAppliedTicks(null);
      void acted
        .attempt(async () => {
          const result = await fetchLiveAndReconcile(acct, inst, run, repository);
          setReport(result);
        })
        .finally(() => setRunning(false));
    },
    // acted is stable per render pattern
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [repository],
  );

  useEffect(() => {
    if (!autoStart || autoStarted.current || !accountId) return;
    autoStarted.current = true;
    execute(accountId, instanceId, runId);
  }, [autoStart, accountId, instanceId, runId, execute]);

  const recommendedTicks = useMemo(() => {
    const fills = report?.summary.fills;
    if (!fills || fills.slippage_samples === 0) return null;
    const basis = fills.p90_slippage_ticks ?? fills.mean_slippage_ticks;
    if (basis === null || !Number.isFinite(basis)) return null;
    return Math.max(0, Math.ceil(basis));
  }, [report]);

  const applySlippage = () => {
    if (recommendedTicks === null) return;
    const saved = readConditions();
    const base =
      saved.current ??
      saved.items.find(item => item.name === saved.active)?.fields ??
      scopeOf(defaultForm());
    saveConditions({
      ...saved,
      current: { ...base, slippage: String(recommendedTicks) },
    });
    setAppliedTicks(recommendedTicks);
  };

  const barsMetric = report ? matchRate(report.summary.bars) : null;
  const recordedMetric = report ? matchRate(report.summary.recorded_bars) : null;
  const decisionsMetric = report ? matchRate(report.summary.decisions) : null;

  return (
    <section className="lab-runs-view">
      <header className="lab-report-header">
        <button type="button" className="lab-link" onClick={back}>
          {t("← 回测记录")}
        </button>
        <h2>{t("实盘对账")}</h2>
        {report && (
          <StatusBadge tone={report.ok ? "ok" : "attention"}>
            {t(report.ok ? "对账通过" : "发现差异")}
          </StatusBadge>
        )}
        <span className="lab-spacer" />
        {report?.metadata.run_id && openRun && (
          <button
            type="button"
            className="lab-link"
            onClick={() => openRun(report.metadata.run_id)}
          >
            {t("查看对照回测")}
          </button>
        )}
      </header>
      <div className="lab-toolbar">
        <select
          aria-label={t("交易账户")}
          value={accountId}
          disabled={running || accounts.length === 0}
          onChange={event => setAccountId(event.target.value)}
        >
          {accounts.map(account => (
            <option key={account.id} value={account.id}>
              {account.instance || account.id}
            </option>
          ))}
        </select>
        <input
          type="text"
          aria-label={t("策略实例")}
          placeholder={t("留空取最近实例")}
          spellCheck={false}
          value={instanceId}
          disabled={running}
          onChange={event => setInstanceId(event.target.value)}
        />
        <input
          type="text"
          aria-label={t("对照回测")}
          placeholder={t("自动匹配来源回测")}
          spellCheck={false}
          value={runId}
          disabled={running}
          onChange={event => setRunId(event.target.value)}
        />
        <button
          type="button"
          className="primary"
          disabled={running || !accountId}
          onClick={() => execute(accountId, instanceId, runId)}
        >
          {running ? t("对账中…") : t("开始对账")}
        </button>
      </div>
      <ActionError error={acted.error} namespace="asterion.terminal.research" />
      {running && !report && (
        <p className="subtle" role="status">
          {t("正在拉取实盘记录并重放回测…")}
        </p>
      )}
      {!running && !report && !acted.error && (
        <p className="lab-placeholder">
          {t(
            accounts.length
              ? "选择交易账户并开始对账，核对实盘 K 线、目标仓位与成交滑点。"
              : "请先在设置中连接至少一个交易账户。",
          )}
        </p>
      )}
      {report && (
        <div className="lab-result">
          <p className="subtle">
            {report.metadata.instance_id} · {report.metadata.package} ·{" "}
            {report.metadata.symbols.join(listSeparator())} · {report.metadata.window_begin_day} –{" "}
            {report.metadata.window_end_day} · {report.metadata.period} ·{" "}
            {report.metadata.dataset_version.slice(0, 12)}
          </p>
          <dl className="lab-kpis">
            <div>
              <dt>{t("输入 K 线一致率")}</dt>
              <dd className="numeric">{barsMetric?.rate}</dd>
              <dd className="subtle numeric">{barsMetric?.detail}</dd>
            </div>
            <div>
              <dt>{t("录制 K 线一致率")}</dt>
              <dd className="numeric">{recordedMetric?.rate}</dd>
              <dd className="subtle numeric">{recordedMetric?.detail}</dd>
            </div>
            <div>
              <dt>{t("信号一致率")}</dt>
              <dd className="numeric">{decisionsMetric?.rate}</dd>
              <dd className="subtle numeric">{decisionsMetric?.detail}</dd>
            </div>
            <div>
              <dt>{t("实盘滑点（均值 / P90）")}</dt>
              <dd className="numeric">
                {report.summary.fills.slippage_samples > 0
                  ? t("{mean} / {p90} 跳", {
                      mean: ticksText(report.summary.fills.mean_slippage_ticks),
                      p90: ticksText(report.summary.fills.p90_slippage_ticks),
                    })
                  : "—"}
              </dd>
              {report.summary.fills.slippage_samples > 0 && (
                <dd className="subtle numeric">
                  {t("中位 {median} · 最大 {max} · {count} 组", {
                    median: ticksText(report.summary.fills.median_slippage_ticks),
                    max: ticksText(report.summary.fills.max_slippage_ticks),
                    count: report.summary.fills.slippage_samples,
                  })}
                </dd>
              )}
            </div>
          </dl>
          {recommendedTicks !== null && (
            <div className="lab-toolbar">
              <button type="button" onClick={applySlippage}>
                {t("应用实盘滑点到回测条件（{ticks} 跳）", { ticks: recommendedTicks })}
              </button>
              {appliedTicks !== null && (
                <span className="subtle" role="status">
                  {t("已将回测条件滑点更新为 {ticks} 跳", { ticks: appliedTicks })}
                </span>
              )}
            </div>
          )}
          {report.ok && (
            <p className="subtle" role="status">
              {t("输入 K 线、录制 K 线、目标仓位与成交明细全部一致。")}
            </p>
          )}
          {report.bar_diffs.length > 0 && (
            <section>
              <h3>{t("策略输入 K 线差异（{count}）", { count: report.bar_diffs.length })}</h3>
              <table className="lab-table">
                <thead>
                  <tr>
                    <th scope="col">{t("交易日")}</th>
                    <th scope="col">{t("品种")}</th>
                    <th scope="col">{t("类型")}</th>
                    <th scope="col">{t("说明")}</th>
                  </tr>
                </thead>
                <tbody>
                  {report.bar_diffs.slice(0, 50).map((diff, index) => (
                    <tr key={`${diff.end_ms}-${diff.symbol}-${index}`}>
                      <td>{diff.trading_day}</td>
                      <td>{diff.symbol}</td>
                      <td>{diffKindText(diff.kind)}</td>
                      <td className="subtle">
                        {diff.fields
                          ? Object.entries(diff.fields)
                              .map(
                                ([field, item]) =>
                                  `${field}: ${item.live ?? "—"} / ${item.backtest ?? "—"}`,
                              )
                              .join(" · ")
                          : (diff.live_close ?? diff.backtest_close ?? "—")}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </section>
          )}
          {report.recorded_bar_diffs.length > 0 && (
            <section>
              <h3>{t("录制 K 线差异（{count}）", { count: report.recorded_bar_diffs.length })}</h3>
              <table className="lab-table">
                <thead>
                  <tr>
                    <th scope="col">{t("交易日")}</th>
                    <th scope="col">{t("合约")}</th>
                    <th scope="col">{t("类型")}</th>
                    <th scope="col">{t("说明")}</th>
                  </tr>
                </thead>
                <tbody>
                  {report.recorded_bar_diffs.slice(0, 50).map((diff, index) => (
                    <tr key={`${diff.end_ms}-${diff.symbol}-${index}`}>
                      <td>{diff.trading_day}</td>
                      <td>{diff.symbol}</td>
                      <td>{diffKindText(diff.kind)}</td>
                      <td className="subtle">
                        {diff.fields
                          ? Object.entries(diff.fields)
                              .map(
                                ([field, item]) =>
                                  `${field}: ${item.live ?? "—"} / ${item.backtest ?? "—"}`,
                              )
                              .join(" · ")
                          : (diff.live_close ?? diff.backtest_close ?? "—")}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </section>
          )}
          {report.decision_diffs.length > 0 && (
            <section>
              <h3>{t("目标仓位差异（{count}）", { count: report.decision_diffs.length })}</h3>
              <table className="lab-table">
                <thead>
                  <tr>
                    <th scope="col">{t("批次")}</th>
                    <th scope="col">{t("类型")}</th>
                    <th scope="col">{t("归因")}</th>
                    <th scope="col">{t("说明")}</th>
                  </tr>
                </thead>
                <tbody>
                  {report.decision_diffs.slice(0, 50).map((diff, index) => (
                    <tr key={`${diff.end_ms}-${index}`}>
                      <td className="numeric">{diff.sequence ?? "—"}</td>
                      <td>{diffKindText(diff.kind)}</td>
                      <td>{diff.caused_by_bar_diff ? t("因 K 线差异引起") : t("策略决策差异")}</td>
                      <td className="subtle">
                        {[
                          diff.ready
                            ? `ready: ${String(diff.ready.live)} / ${String(diff.ready.backtest)}`
                            : "",
                          diff.targets
                            ? Object.entries(diff.targets)
                                .map(
                                  ([sym, item]) =>
                                    `${sym}: ${item.live ?? "—"} / ${item.backtest ?? "—"}`,
                                )
                                .join(" · ")
                            : "",
                        ]
                          .filter(Boolean)
                          .join(" · ") || "—"}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </section>
          )}
          {report.fill_diffs.length > 0 && (
            <section>
              <h3>{t("成交与滑点差异（{count}）", { count: report.fill_diffs.length })}</h3>
              <table className="lab-table">
                <thead>
                  <tr>
                    <th scope="col">{t("交易日")}</th>
                    <th scope="col">{t("合约")}</th>
                    <th scope="col">{t("方向")}</th>
                    <th scope="col">{t("开平")}</th>
                    <th scope="col">{t("类型")}</th>
                    <th scope="col" className="numeric">
                      {t("实盘 / 回测手数")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("实盘 / 回测均价")}
                    </th>
                    <th scope="col" className="numeric">
                      {t("不利滑点（跳）")}
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {report.fill_diffs.slice(0, 50).map((diff, index) => (
                    <tr
                      key={`${diff.trading_day}-${diff.contract}-${diff.side}-${diff.offset}-${index}`}
                    >
                      <td>{diff.trading_day}</td>
                      <td>{diff.contract}</td>
                      <td>{sideText(diff.side)}</td>
                      <td>{offsetText(diff.offset)}</td>
                      <td>{diffKindText(diff.kind)}</td>
                      <td className="numeric">
                        {diff.live_qty ?? "—"} / {diff.backtest_qty ?? "—"}
                      </td>
                      <td className="numeric">
                        {diff.live_vwap ?? "—"} / {diff.backtest_vwap ?? "—"}
                      </td>
                      <td className="numeric">
                        {diff.adverse_ticks != null ? ticksText(diff.adverse_ticks) : "—"}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </section>
          )}
        </div>
      )}
    </section>
  );
}
