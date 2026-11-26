import { create } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { Period } from "../../src/generated/asterion/common_pb";
import {
  FactorDaySchema,
  FactorEntrySchema,
  FactorResultSchema,
  RunDaySchema,
  RunDetailSchema,
  ParameterSchema,
  RunSummarySchema,
} from "../../src/generated/asterion/lab_pb";
import { registerLanguageResources, setLocale } from "../../src/i18n";
import { feature } from "./feature";
import {
  breakEvenSlippage,
  comparePortfolio,
  computeDrawdowns,
  cumulativeIc,
  deflatedSharpeRatio,
  evaluateRunVerdicts,
  factorTurnover,
  heatmap,
  metricDeltaText,
  monthlyReturns,
  returnHistogram,
  yearlyIc,
} from "./research-analytics";
import {
  defaultFactorForm,
  defaultForm,
  factorRequest,
  formFromRun,
  identity,
  onScope,
  parameterValues,
  runRequest,
  sameScope,
  saveConditions,
  savedForm,
  scopeOf,
} from "./research-lab";
import { formatRunSummaryMarkdown } from "./research-markdown";
import { readAnnotations, saveAnnotation } from "./research-storage";
import {
  combinedStrategyTemplate,
  factorStrategyStem,
  factorStrategyTemplate,
} from "./research-templates";

registerLanguageResources(feature.id, feature.languageResources!);
setLocale("zh-CN");
const integer = (name: string, value: bigint) =>
  create(ParameterSchema, {
    name,
    default: { value: { case: "integer", value } },
  });

test("a field's values follow the parameter's kind, and several make a sweep", () => {
  expect(parameterValues(integer("fast", 5n), "3, 5，8").map(v => v.value)).toEqual([
    { case: "integer", value: 3n },
    { case: "integer", value: 5n },
    { case: "integer", value: 8n },
  ]);
  expect(() => parameterValues(integer("fast", 5n), "3.5")).toThrow("不是整数");
  expect(() => parameterValues(integer("fast", 5n), "")).toThrow("没有值");
  const form = { ...defaultForm(), symbols: "SHFE/rb SHFE/au/2026-12", params: { fast: "3,5" } };
  const { run, swept } = runRequest(
    form,
    "/repo",
    "ma_cross.MaCross",
    [integer("fast", 5n), integer("slow", 20n)],
    "x",
  );
  expect(swept.map(item => item.name)).toEqual(["fast"]);
  expect(run.params).toEqual({ slow: { value: { case: "integer", value: 20n } } });
});

test("monthly returns compound from the deposit, and a sweep grid fills by its two parameters", () => {
  const days = [
    ["2026-01-05", "1010000"],
    ["2026-01-30", "1020000"],
    ["2026-02-02", "1000000"],
    ["2026-02-27", "1050000"],
  ].map(([tradingDay, equity]) => create(RunDaySchema, { tradingDay, equity: { text: equity } }));
  const months = monthlyReturns("1000000", days);
  expect(months.map(m => m.month)).toEqual(["2026-01", "2026-02"]);
  expect(months[0].value).toBeCloseTo(0.02);
  expect(months[1].value).toBeCloseTo(1050000 / 1020000 - 1);
  const blownUp = [
    ["2026-01-30", "-50000"],
    ["2026-02-27", "-80000"],
  ].map(([tradingDay, equity]) => create(RunDaySchema, { tradingDay, equity: { text: equity } }));
  expect(monthlyReturns("1000000", blownUp)).toEqual([{ month: "2026-01", value: -1 }]);
  const grid = heatmap(
    [
      { params: { fast: "10", slow: "20" }, metric: 1 },
      { params: { fast: "5", slow: "20" }, metric: 2 },
      { params: { fast: "5", slow: "30" }, metric: null },
    ],
    "fast",
    "slow",
  );
  expect(grid).toEqual({
    rows: ["5", "10"],
    columns: ["20", "30"],
    cells: [
      [2, null],
      [1, null],
    ],
  });
});

test("a factor's evaluation takes one value of each setting, horizons and groups", () => {
  const momentum = create(FactorEntrySchema, {
    id: "price.momentum",
    inputs: ["close"],
    parameters: [integer("lookback", 20n)],
  });
  const form = { ...defaultFactorForm(), symbols: "SHFE, DCE/i", begin: "2025-01-02" };
  const request = factorRequest(form, "/repo", momentum);
  expect(request.symbols).toEqual(["SHFE", "DCE/i"]);
  expect(request.params).toEqual({ lookback: { value: { case: "integer", value: 20n } } });
  expect(request.horizons).toEqual([1, 2, 3, 5, 10, 20]);
  expect(request.groups).toBe(5);
  expect(request.returnMode).toBe("open");
  expect(() =>
    factorRequest({ ...form, params: { lookback: "10, 20" } }, "/repo", momentum),
  ).toThrow("只能取一个值");
  expect(() => factorRequest({ ...form, horizons: "1, 1" }, "/repo", momentum)).toThrow("互不相同");
  expect(() => factorRequest({ ...form, groups: "1" }, "/repo", momentum)).toThrow("至少为 2");
  expect(() => factorRequest({ ...form, costBps: "-1" }, "/repo", momentum)).toThrow("非负数");
  const realistic = factorRequest(
    {
      ...form,
      winsorize: true,
      neutralize: "sector",
      minVolume: "1000",
      minOpenInterest: "5000",
      volAdjust: true,
      costBps: "2.5",
      orthogonalizeAgainst: ["price.momentum", "positions.interest_change"],
    },
    "/repo",
    momentum,
  );
  expect(realistic.winsorize).toBe(true);
  expect(realistic.neutralize).toBe("sector");
  expect(realistic.minVolume).toBe(1000);
  expect(realistic.minOpenInterest).toBe(5000);
  expect(realistic.volAdjust).toBe(true);
  expect(realistic.costBps).toBe(2.5);
  expect(realistic.orthogonalizeAgainst).toEqual([
    { factor: "positions.interest_change", params: {} },
  ]);
});

test("the cumulative IC adds nothing on a day without one", () => {
  const days = [0.1, undefined, -0.05].map(ic => create(FactorDaySchema, { ic }));
  expect(cumulativeIc(days).map(value => Number(value.toFixed(6)))).toEqual([0.1, 0.1, 0.05]);
});

test("factorTurnover averages finite group turnovers and yearlyIc breaks down IC by year", () => {
  expect(factorTurnover(undefined)).toBeUndefined();
  expect(
    factorTurnover(
      create(FactorResultSchema, {
        groups: [{ group: 1, turnover: 0.2 }, { group: 2 }, { group: 3, turnover: 0.4 }],
      }),
    ),
  ).toBeCloseTo(0.3);

  const days = [
    create(FactorDaySchema, { tradingDay: "2024-06-03", ic: 0.2 }),
    create(FactorDaySchema, { tradingDay: "2025-01-02", ic: 0.1 }),
    create(FactorDaySchema, { tradingDay: "2025-01-03", ic: 0.3 }),
    create(FactorDaySchema, { tradingDay: "2025-01-06" }),
    create(FactorDaySchema, { tradingDay: "2025-01-07", ic: -0.1 }),
  ];
  const breakdown = yearlyIc(days, 3);
  expect(breakdown.map(row => row.year)).toEqual(["2024", "2025"]);
  expect(breakdown[0]).toEqual({
    year: "2024",
    days: 1,
    mean: 0.2,
    deviation: undefined,
    ir: undefined,
    t: undefined,
    positiveShare: 1,
  });
  expect(breakdown[1]!.days).toBe(3);
  expect(breakdown[1]!.mean).toBeCloseTo(0.1);
  expect(breakdown[1]!.deviation).toBeCloseTo(0.2);
  expect(breakdown[1]!.ir).toBeCloseTo(0.5);
  expect(breakdown[1]!.t).toBeCloseTo(0.5 * Math.sqrt(3 / 3));
  expect(breakdown[1]!.positiveShare).toBeCloseTo(2 / 3);
});

test("batch and composite factor requests apply defaults, override extras, and bound id length", () => {
  const momentum = create(FactorEntrySchema, {
    id: "price.momentum",
    inputs: ["close"],
    parameters: [integer("lookback", 20n)],
  });
  const form = {
    ...defaultFactorForm(),
    symbols: "SHFE",
    params: { lookback: "10, 20" },
    extraFactors: ["price.reversal"],
    combination: "ic_weighted" as const,
  };
  const batchReq = factorRequest(form, "/repo", momentum, { useDefaults: true, extraFactors: [] });
  expect(batchReq.params).toEqual({ lookback: { value: { case: "integer", value: 20n } } });
  expect(batchReq.extraFactors).toEqual([]);
  expect(batchReq.combination).toBe("");

  const mixReq = factorRequest(form, "/repo", momentum, {
    useDefaults: true,
    extraFactors: ["price.momentum", "positions.interest_change"],
  });
  expect(mixReq.extraFactors).toEqual([{ factor: "positions.interest_change", params: {} }]);
  expect(mixReq.combination).toBe("ic_weighted");

  const longId = identity(`${"a".repeat(60)}.factor`, "mix");
  expect(longId.length).toBeLessThanOrEqual(64);
  expect(longId.endsWith("-mix")).toBe(true);
});

test("formFromRun restores strategy, parameters, period, limits and validation from a run record", () => {
  const detail = create(RunDetailSchema, {
    record: JSON.stringify({
      strategy: { id: "ma_cross.MaCross" },
      params: { fast: 5, slow: 20 },
      data: {
        symbols: ["SHFE/rb", "DCE/i"],
        begin_day: "2026-01-05",
        end_day: "2026-02-13",
        bars: { period: "15m" },
      },
      costs: { deposit: "500000", slippage_ticks: 2 },
      risk: {
        max_order_lots: "4",
        max_contract_lots: "0",
        max_gross_lots: "12",
        max_working_orders: 8,
        max_gross_weight: "1.5",
        max_net_weight: "0.5",
      },
      statistics: { annual_periods: 252, zero_risk_free: false, holdout_day: "2026-02-02" },
    }),
  });
  expect(formFromRun(detail)).toEqual({
    strategy: "ma_cross.MaCross",
    form: {
      ...defaultForm(),
      symbols: "SHFE/rb, DCE/i",
      multiSymbolMode: "portfolio",
      begin: "2026-01-05",
      end: "2026-02-13",
      minutes: "15",
      params: { fast: "5", slow: "20" },
      deposit: "500000",
      slippage: "2",
      maxOrderLots: "4",
      maxContractLots: "",
      maxGrossLots: "12",
      maxWorkingOrders: "8",
      maxGrossWeight: "1.5",
      maxNetWeight: "0.5",
      annualPeriods: "252",
      zeroRiskFree: false,
      validation: "holdout",
      holdoutDay: "2026-02-02",
    },
  });
});

const measured = (settings: Record<string, unknown> = {}) =>
  create(RunSummarySchema, {
    symbols: ["DCE/i", "SHFE/rb"],
    firstDay: "2024-01-02",
    lastDay: "2026-02-13",
    period: Period.DAY,
    settings: {
      deposit: { text: "1000000.00" },
      slippageTicks: 1,
      limits: {
        maxOrderLots: { text: "100" },
        maxContractLots: { text: "500" },
        maxGrossLots: { text: "2000" },
        maxWorkingOrders: 64,
      },
      maxGrossWeight: { text: "1" },
      maxNetWeight: { text: "1" },
      maxMarginRatio: { text: "0" },
      minRebalanceWeight: { text: "0" },
      periodsPerYear: 252,
      zeroRiskFreeRate: true,
      ...settings,
    },
  });

test("a run is on the settings when every measured value agrees", () => {
  const form = { ...defaultForm(), symbols: "SHFE/rb, DCE/i" };
  expect(onScope(form, measured(), "2024-01-02", "2026-02-13")).toBe(true);
  // Days not yet known cannot tell runs apart.
  expect(onScope(form, measured(), "YYYY-MM-DD", "YYYY-MM-DD")).toBe(true);
  expect(onScope(form, measured(), "2024-01-03", "2026-02-13")).toBe(false);
  expect(onScope(form, measured({ slippageTicks: 2 }), "2024-01-02", "2026-02-13")).toBe(false);
  expect(onScope(form, measured({ holdoutDay: "2025-01-02" }), "2024-01-02", "2026-02-13")).toBe(
    false,
  );
  expect(onScope({ ...form, symbols: "SHFE/rb" }, measured(), "2024-01-02", "2026-02-13")).toBe(
    false,
  );
});

test("the settings outlive a restart, and a strategy's parameters do not", () => {
  localStorage.clear();
  saveConditions({
    active: "黑色",
    items: [
      { name: "全市场", fields: scopeOf({ ...defaultForm(), symbols: "DCE/i" }) },
      {
        name: "黑色",
        fields: scopeOf({ ...defaultForm(), symbols: "SHFE/rb", deposit: "500000" }),
      },
    ],
  });
  const form = savedForm();
  expect(form.symbols).toBe("SHFE/rb");
  expect(form.deposit).toBe("500000");
  expect(form.params).toEqual({});
});

test("two sets of settings are the same when their values are", () => {
  const base = scopeOf({ ...defaultForm(), symbols: "SHFE/rb, DCE/i", deposit: "1000000" });
  expect(sameScope(base, { ...base, symbols: "DCE/i, SHFE/rb", deposit: "1000000.00" })).toBe(true);
  expect(sameScope(base, { ...base, slippage: "2" })).toBe(false);
  expect(sameScope(base, { ...base, begin: "2026-01-05" })).toBe(false);
});

test("factorStrategyTemplate generates deployable Python strategy code preserving hygiene and params", () => {
  const result = create(FactorResultSchema, {
    id: "mix-1",
    evaluation: {
      factor: "price.momentum",
      params: { lookback: { value: { case: "integer", value: 30n } } },
      extraFactors: [{ factor: "positions.interest_change", params: {} }],
      orthogonalizeAgainst: [
        {
          factor: "controls.momentum",
          params: { lookback: { value: { case: "integer", value: 40n } } },
        },
        {
          factor: "controls.orthogonalize",
          params: { window: { value: { case: "integer", value: 25n } } },
        },
      ],
      horizons: [5, 10],
      groups: 3,
      winsorize: true,
      standardize: "rank",
      neutralize: "sector",
      minVolume: 500,
      minOpenInterest: 2000,
    },
    factorNames: ["price.momentum", "positions.interest_change"],
    symbols: ["SHFE/rb", "DCE/i", "SHFE/cu", "SHFE/al", "SHFE/zn", "SHFE/au"],
    horizons: [{ horizon: 5, mean: -0.08 }],
  });
  const stem = factorStrategyStem(result);
  expect(stem).toBe("momentum_interest_change_strategy");
  const built = factorStrategyTemplate(result, stem);
  expect(built.id).toBe("momentum_interest_change_strategy.MomentumInterestChangeStrategy");
  expect(built.content).toContain("from factors.price import momentum");
  expect(built.content).toContain("from factors.positions import interest_change");
  expect(built.content).toContain(
    "from factors.controls import momentum as controls_momentum, orthogonalize as controls_orthogonalize",
  );
  expect(built.content).toContain("ctx.factor(momentum, s, lookback=30)");
  expect(built.content).toContain("ctx.factor(interest_change, s)");
  expect(built.content).toContain("ctx.factor(controls_momentum, s, lookback=40)");
  expect(built.content).toContain("ctx.factor(controls_orthogonalize, s, window=25)");
  expect(built.content).toContain("scores = orthogonalize(scores, controls)");
  expect(built.content.indexOf("scores = orthogonalize(scores, controls)")).toBeGreaterThan(
    built.content.indexOf("scores = sector_neutralize(scores)"),
  );
  expect(built.content.match(/components.append\(/g)).toHaveLength(2);
  expect(built.content).toContain("winsorize_mad");
  expect(built.content).toContain("cross_section_rank");
  expect(built.content).toContain("sector_neutralize");
  expect(built.content).toContain("cap_sector_weights");
  expect(built.content).toContain("smooth_weights");
  expect(built.content).toContain('"direction": -1');
  expect(built.content).toContain('"min_volume": 500.0');
  expect(built.content).toContain("lookback=30");
  expect(built.content).toContain("def ready(self, ctx):");
});

test("factor strategies use retained history for default and long factor windows", () => {
  for (const extraFactors of [[], [{ factor: "price.skip_momentum", params: {} }]]) {
    const result = create(FactorResultSchema, {
      evaluation: {
        factor: "price.momentum",
        params: { lookback: { value: { case: "integer", value: 600n } } },
        extraFactors,
      },
    });
    const { content } = factorStrategyTemplate(result, "long_momentum");
    expect(content).toContain("ctx.factor(momentum, s, lookback=600)");
    if (extraFactors.length) expect(content).toContain("ctx.factor(skip_momentum, s)");
    expect(content).toContain('self.need = p["volatility"] + 1');
    expect(content).toContain(
      'ctx.factor(_volatility, s, p["volatility"] + 1, window=p["volatility"])',
    );
    expect(content).not.toContain("history_limit");
  }
});

test("DSR, break-even slippage, portfolio comparison, and combinedStrategyTemplate compute accurate strategy analytics", () => {
  // Higher trial dispersion lowers DSR confidence; strong Sharpe with many observations gives high DSR.
  const strong = deflatedSharpeRatio(2.2, [0.4, 0.8, 1.2, 2.2], 504)!;
  const noisy = deflatedSharpeRatio(0.9, [-0.5, 0.0, 0.4, 0.9, 0.8, 0.7, 0.2, 0.1], 60)!;
  expect(strong.dsr).toBeGreaterThan(0.9);
  expect(noisy.dsr).toBeLessThan(strong.dsr);

  // Break-even slippage interpolates zero crossing and reports above_max when still positive.
  expect(
    breakEvenSlippage([
      { ticks: 0, sharpe: 1.0 },
      { ticks: 2, sharpe: 0.5 },
      { ticks: 4, sharpe: -0.5 },
    ]),
  ).toEqual({ ticks: 3, status: "interpolated", maxTicks: 4 });
  expect(
    breakEvenSlippage([
      { ticks: 0, sharpe: 1.5 },
      { ticks: 5, sharpe: 0.4 },
    ]),
  ).toEqual({ ticks: null, status: "above_max", maxTicks: 5 });

  // Portfolio comparison computes correlation, inverse-vol weights, volatility reduction, and CombinedStrategy code.
  const makeRun = (id: string, strategy: string, equities: number[], params: object) =>
    create(RunDetailSchema, {
      summary: { id, strategy, symbols: ["SHFE/rb", "SHFE/cu"] },
      record: JSON.stringify({ strategy: { id: strategy }, params, costs: { deposit: "100" } }),
      days: equities.map((eq, idx) => ({
        tradingDay: `2026-01-0${idx + 1}`,
        equity: { text: String(eq) },
        settledMs: BigInt((idx + 1) * 86400_000),
      })),
    });
  const a = makeRun("r1", "trend.Trend", [100, 102, 101, 103, 102, 105], { lookback: 20 });
  const b = makeRun("r2", "carry.Carry", [100, 96, 104, 98, 106, 102], { window: 10 });
  const sim = comparePortfolio([a, b], "inv_vol")!;
  expect(sim.days).toHaveLength(6);
  expect(sim.correlations[0]![1]!).toBeLessThan(0);
  // Lower-volatility strategy `a` gets higher weight under inverse-vol parity.
  expect(sim.weights[0]!).toBeGreaterThan(sim.weights[1]!);
  expect(sim.volReduction!).toBeGreaterThan(0);

  const generated = combinedStrategyTemplate([a, b], sim.weights, true, "trend_carry_portfolio");
  expect(generated.id).toBe("trend_carry_portfolio.TrendCarryPortfolio");
  expect(generated.content).toContain("from strategies.trend import Trend");
  expect(generated.content).toContain("from strategies.carry import Carry");
  expect(generated.content).toContain("vol_balance = True");
  expect(generated.content).toContain("Trend(lookback=20)");
  expect(generated.content).toContain("Carry(window=10)");
  expect(generated.content).toContain('source_runs = ("r1", "r2")');
});

test("experiment annotations, verdicts, drawdowns, histogram, deltas, and AI summary markdown work end-to-end", () => {
  localStorage.clear();
  saveAnnotation("run-1", { starred: true, note: "  robust at 2 ticks  " });
  expect(readAnnotations()["run-1"]).toEqual({ starred: true, note: "robust at 2 ticks" });
  saveAnnotation("run-1", { starred: false, note: "" });
  expect(readAnnotations()["run-1"]).toBeUndefined();

  expect(metricDeltaText(0.125, 0.1, true)).toBe("+2.50%");
  expect(metricDeltaText(1.1, 1.35, false)).toBe("-0.250");
  expect(metricDeltaText(1.0, 1.0, false)).toBe("");

  const days = [100, 110, 99, 105, 115, 100, 102].map((eq, i) => ({
    tradingDay: `2026-01-0${i + 1}`,
    equity: { text: String(eq) },
    balance: { text: String(eq) },
    fees: { text: "0" },
    margin: { text: "0" },
    settledMs: BigInt((i + 1) * 86400_000),
  }));
  const dds = computeDrawdowns(days as never, 100);
  expect(dds).toHaveLength(2);
  expect(dds[0]!.peak_day).toBe("2026-01-05");
  expect(dds[0]!.trough_day).toBe("2026-01-06");
  expect(dds[0]!.recovery_day).toBeNull();
  expect(dds[0]!.depth).toBeCloseTo(100 / 115 - 1, 5);
  expect(dds[1]!.recovery_day).toBe("2026-01-05");

  const hist = returnHistogram([-0.03, -0.01, 0.0, 0.005, 0.01, 0.02, 0.04], 5)!;
  expect(hist.total).toBe(7);
  expect(hist.bins.reduce((acc, b) => acc + b.count, 0)).toBe(7);
  expect(hist.p05).toBeLessThan(0);

  const summary = create(RunDetailSchema, {
    summary: {
      id: "run-verdict",
      strategy: "trend.Trend",
      symbols: ["SHFE/rb", "SHFE/cu"],
      metrics: {
        total_return: 0.12,
        sharpe: 1.4,
        development_sharpe: 1.8,
        holdout_sharpe: 0.4,
        fills: 20,
      },
    },
    record: JSON.stringify({
      strategy: { id: "trend.Trend", commit: "abcdef123456" },
      params: { lookback: 20 },
      costs: { deposit: "1000000", slippage_ticks: 1 },
      data: {
        dataset_version: "1234567890abcdef",
        symbols: ["SHFE/rb", "SHFE/cu"],
        begin_day: "2026-01-01",
        end_day: "2026-06-01",
        bars: { period: "day" },
        rejections: [{ count: 2 }],
        diagnostics: {
          costs: { gross_pnl: 1000, fees: 300, estimated_slippage: 200, net_pnl: 500 },
          by_symbol: [
            { symbol: "SHFE/rb", net_pnl: 450, fills: 12 },
            { symbol: "SHFE/cu", net_pnl: 50, fills: 8 },
          ],
          trials: { strategy_runs: 35, holdout_runs: 18, sharpe_std: 0.4 },
          uses_pit_fundamentals: true,
        },
      },
    }),
  });
  const verdicts = evaluateRunVerdicts(summary.summary!, JSON.parse(summary.record), 100);
  expect(verdicts.map(v => v.label)).toEqual([
    "留出衰减",
    "成本偏高",
    "品种集中",
    "存在拒单",
    "高频试错",
    "PIT 基本面",
  ]);
  const md = formatRunSummaryMarkdown(summary, JSON.parse(summary.record), "holdout decayed");
  expect(md).toContain("## Backtest:");
  expect(md).toContain("holdout decayed");
  expect(md).toContain("Trials: 35");
});

test("feeMultiplier round-trips through runRequest, formFromRun, sameScope, and onScope", () => {
  const form = { ...defaultForm(), symbols: "SHFE/rb", feeMultiplier: "1.5" };
  const { run } = runRequest(form, "/repo", "ma_cross.MaCross", [], "r-fee");
  expect(run.feeMultiplier?.text).toBe("1.5");
  expect(sameScope(scopeOf(form), scopeOf({ ...form, feeMultiplier: "1.50" }))).toBe(true);
  expect(sameScope(scopeOf(form), scopeOf({ ...form, feeMultiplier: "1" }))).toBe(false);
  expect(
    sameScope(scopeOf({ ...form, feeMultiplier: "" }), scopeOf({ ...form, feeMultiplier: "1" })),
  ).toBe(true);

  const detail = create(RunDetailSchema, {
    summary: {
      id: "r-fee",
      strategy: "ma_cross.MaCross",
      symbols: ["SHFE/rb"],
      firstDay: "2026-01-01",
      lastDay: "2026-01-31",
      period: Period.DAY,
      settings: {
        deposit: { text: "1000000" },
        slippageTicks: 1,
        feeMultiplier: { text: "1.5" },
        limits: {
          maxOrderLots: { text: "100" },
          maxContractLots: { text: "500" },
          maxGrossLots: { text: "2000" },
          maxWorkingOrders: 64,
        },
        maxGrossWeight: { text: "1" },
        maxNetWeight: { text: "1" },
        periodsPerYear: 252,
        zeroRiskFreeRate: true,
      },
    },
    record: JSON.stringify({
      strategy: { id: "ma_cross.MaCross" },
      params: {},
      data: {
        symbols: ["SHFE/rb"],
        begin_day: "2026-01-01",
        end_day: "2026-01-31",
        bars: { period: "day" },
      },
      costs: { deposit: "1000000", slippage_ticks: 1, fee_multiplier: "1.5" },
      risk: {
        max_order_lots: "100",
        max_contract_lots: "500",
        max_gross_lots: "2000",
        max_working_orders: 64,
      },
      statistics: { annual_periods: 252, zero_risk_free: true, holdout_day: "" },
    }),
  });
  expect(onScope(form, detail.summary!, "2026-01-01", "2026-01-31")).toBe(true);
  expect(
    onScope({ ...form, feeMultiplier: "2" }, detail.summary!, "2026-01-01", "2026-01-31"),
  ).toBe(false);
  expect(formFromRun(detail).form.feeMultiplier).toBe("1.5");
});
