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
import { setLocale } from "../../src/i18n";
import "./feature";
import {
  cumulativeIc,
  defaultFactorForm,
  defaultForm,
  factorRequest,
  factorTurnover,
  formFromRun,
  heatmap,
  identity,
  monthlyReturns,
  onScope,
  parameterValues,
  runRequest,
  sameScope,
  saveConditions,
  savedForm,
  scopeOf,
  yearlyIc,
} from "./research-lab";

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
  expect(() =>
    factorRequest({ ...form, params: { lookback: "10, 20" } }, "/repo", momentum),
  ).toThrow("只能取一个值");
  expect(() => factorRequest({ ...form, horizons: "1, 1" }, "/repo", momentum)).toThrow("互不相同");
  expect(() => factorRequest({ ...form, groups: "1" }, "/repo", momentum)).toThrow("至少为 2");
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
