import { fireEvent, render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import {
  FactorResultSchema,
  FactorResultsSchema,
  LabActivitySchema,
  RunState,
  SourceSchema,
  StrategiesSchema,
  TaskKind,
} from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { LabActivityProvider } from "../../src/host/lab-activity";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { defaultFactorForm, factorScopeOf, saveFactorConditions } from "./research-lab";
import { FactorLab } from "./FactorLab";
import { FactorResultView } from "./FactorResultView";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
  onLabMutation: vi.fn(() => () => {}),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, string | number>) =>
    values ? key.replace(/\{(\w+)\}/g, (_, name: string) => String(values[name] ?? "")) : key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

beforeEach(() => localStorage.clear());

test("correlations distinguish unavailable estimates from zero and show sample coverage", () => {
  render(
    <FactorResultView
      result={create(FactorResultSchema, {
        factorNames: ["a.signal", "b.signal"],
        correlations: [
          { value: 1, days: 5, samples: 20 },
          { days: 0, samples: 0 },
          { value: 0, days: 5, samples: 20 },
          { value: 1, days: 5, samples: 20 },
        ],
      })}
    />,
  );
  const matrix = screen.getByRole("table", { name: "因子相关性" });
  const missing = within(matrix).getByText("—");
  expect(missing.title).toContain("0 个有效交易日，0 个品种日样本");
  expect(missing.style.background).toBe("");
  const zero = within(matrix).getByText("0.000");
  expect(zero.title).toContain("5 个有效交易日，20 个品种日样本");
});

test("factor result view shows intraday period tag, signal autocorr, half-life, and HAC t", () => {
  render(
    <FactorResultView
      result={create(FactorResultSchema, {
        evaluation: {
          factor: "price.momentum",
          firstDay: "2026-01-05",
          lastDay: "2026-01-06",
          period: 2, // PERIOD_MINUTES_5
        },
        symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
        autocorr: 0.85,
        halfLife: 4.265,
        horizons: [
          { horizon: 1, days: 40, mean: 0.08, ir: 0.5, t: 3.1, tHac: 2.7, positiveShare: 0.65 },
        ],
        days: [
          {
            tradingDay: "2026-01-05 09:05",
            symbols: 3,
            ic: 0.1,
            groups: [1.0, 1.01],
            longShort: 1.01,
          },
          {
            tradingDay: "2026-01-05 09:10",
            symbols: 3,
            ic: 0.05,
            groups: [1.0, 1.02],
            longShort: 1.02,
          },
        ],
      })}
    />,
  );
  expect(screen.getByText("5m")).toBeTruthy();
  expect(screen.getByText("信号自相关")).toBeTruthy();
  expect(screen.getByText("0.850")).toBeTruthy();
  expect(screen.getByText("信号半衰期")).toBeTruthy();
  expect(screen.getByText("4.3 根")).toBeTruthy();
  expect(screen.getAllByText("HAC t 值").length).toBeGreaterThanOrEqual(1);
});

test("the factor library comes first, ranks, filters, and opens a factor", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [
            { id: "price.momentum", inputs: ["close"], description: "Return over lookback" },
            { id: "price.reversal", inputs: ["close"], description: "Negated return" },
            {
              id: "positions.interest_change",
              inputs: ["open_interest"],
              description: "OI change",
            },
          ],
          strategies: [
            { id: "momentum_rank.MomentumRank", factorModules: ["price"] },
            { id: "trend.Trend", factorModules: ["price"] },
          ],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [
            {
              id: "mix-1",
              evaluation: {
                factor: "price.momentum",
                extraFactors: [{ factor: "price.reversal", params: {} }],
                combination: "equal",
              },
              factorNames: ["price.momentum", "price.reversal"],
              horizons: [{ horizon: 1, mean: 0.9 }],
            },
            {
              id: "b",
              evaluation: { factor: "price.reversal" },
              horizons: [{ horizon: 1, mean: 0.3 }],
              holdoutHorizons: [{ horizon: 1, mean: 0.05 }],
              groups: [
                { group: 1, turnover: 0.4 },
                { group: 2, turnover: 0.6 },
              ],
            },
            {
              id: "a",
              evaluation: { factor: "price.momentum" },
              horizons: [{ horizon: 1, mean: 0.1 }],
              holdoutHorizons: [{ horizon: 1, mean: 0.18 }],
              groups: [
                { group: 1, turnover: 0.1 },
                { group: 2, turnover: 0.3 },
              ],
            },
          ],
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "def momentum(close): ..." }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  expect(screen.queryByRole("complementary", { name: "评价" })).toBeNull();
  const order = () =>
    within(library)
      .getAllByRole("rowheader")
      .map(header => header.textContent);
  await within(library).findByText("0.300");
  expect(within(library).getByText("0.180")).toBeTruthy();
  expect(within(library).getByText("50.00%")).toBeTruthy();
  expect(within(library).getByText("20.00%")).toBeTruthy();
  // Never evaluated comes after the ranking, and composites after that.
  expect(order()).toEqual(["reversal", "momentum", "interest_change", "momentum + reversal"]);

  // Sorting by holdout IC updates row order, aria-sort, and status announcement.
  fireEvent.click(within(library).getByRole("button", { name: "留出期 IC" }));
  expect(order()).toEqual(["momentum", "reversal", "interest_change", "momentum + reversal"]);
  expect(
    within(library)
      .getByRole("columnheader", { name: /留出期 IC/ })
      .getAttribute("aria-sort"),
  ).toBe("descending");
  expect(screen.getByRole("status").textContent).toBe("已按 留出期 IC降序排序");

  // Search covers names, descriptions and bar fields.
  fireEvent.change(screen.getByLabelText("搜索因子"), { target: { value: "close" } });
  expect(order()).toEqual(["momentum", "reversal"]);

  // A factor with an evaluation opens on it, with its code one tab away.
  fireEvent.click(within(library).getByRole("button", { name: "momentum" }));
  expect(screen.queryByRole("table", { name: "因子库" })).toBeNull();
  const config = await screen.findByRole("complementary", { name: "评价" });
  expect(within(config).getByRole("button", { name: "评价" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "代码" }));
  await screen.findByText("factors/price.py");
  fireEvent.click(screen.getByRole("button", { name: "← 因子库" }));
  await screen.findByRole("table", { name: "因子库" });
});

test("only evaluations on the current settings rank", async () => {
  const fields = factorScopeOf({ ...defaultFactorForm(), symbols: "SHFE/rb", horizons: "1" });
  saveFactorConditions({ active: "螺纹钢", items: [{ name: "螺纹钢", fields }] });
  const evaluation = (factor: string, symbols: string[]) => ({
    factor,
    symbols,
    firstDay: "2026-01-05",
    lastDay: "2026-02-13",
    horizons: [1],
    groups: 5,
    returnMode: "open",
  });
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [{ id: "price.same" }, { id: "price.other" }, { id: "price.none" }],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [
            {
              id: "other",
              evaluation: evaluation("price.other", ["DCE/i"]),
              horizons: [{ horizon: 1, mean: 0.9 }],
            },
            {
              id: "same",
              evaluation: evaluation("price.same", ["SHFE/rb"]),
              horizons: [{ horizon: 1, mean: 0.1 }],
            },
          ],
        }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  await within(library).findByText("0.100");
  expect(
    within(library)
      .getAllByRole("rowheader")
      .map(header => header.textContent),
  ).toEqual(["same", "other", "none"]);
  expect(within(library).getByRole("columnheader", { name: "条件不同" })).toBeTruthy();
  expect(within(library).getByRole("columnheader", { name: "未评价" })).toBeTruthy();
});

test("factor library runs batch evaluation and multi-factor composite evaluation from selected rows", async () => {
  const startedBatches: Array<{
    id?: string;
    evaluations?: Array<{
      factor?: string;
      extraFactors?: Array<{ factor?: string }>;
      params?: Record<string, unknown>;
    }>;
  }> = [];
  const startedFactors: Array<{
    id?: string;
    factor?: string;
    extraFactors?: Array<{ factor?: string }>;
    combination?: string;
  }> = [];
  let listCount = 0;
  let lastCompositeId = "";
  const recentTasks: Array<{
    id: string;
    kind: TaskKind;
    strategy: string;
    total: number;
    done: number;
    succeeded: number;
    state: RunState;
  }> = [];
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [
            {
              id: "price.momentum",
              inputs: ["close"],
              parameters: [
                { name: "lookback", default: { value: { case: "integer", value: 20n } } },
              ],
            },
            { id: "positions.interest_change", inputs: ["open_interest"] },
          ],
        }) as never;
      case "listFactorResults":
        listCount += 1;
        return create(FactorResultsSchema, {
          results: lastCompositeId
            ? [
                {
                  id: lastCompositeId,
                  evaluation: {
                    factor: "price.momentum",
                    extraFactors: [{ factor: "positions.interest_change", params: {} }],
                    combination: "ic_weighted",
                  },
                  factorNames: ["price.momentum", "positions.interest_change"],
                  horizons: [{ horizon: 1, mean: 0.22 }],
                },
              ]
            : [],
        }) as never;
      case "startFactorBatch": {
        const value = operation.value;
        startedBatches.push({ id: value.id, evaluations: value.evaluations });
        recentTasks.unshift({
          id: value.id ?? "",
          kind: TaskKind.FACTOR_BATCH,
          strategy: "",
          total: value.evaluations?.length ?? 0,
          done: value.evaluations?.length ?? 0,
          succeeded: value.evaluations?.length ?? 0,
          state: RunState.SUCCEEDED,
        });
        return {} as never;
      }
      case "startFactor": {
        const value = operation.value;
        startedFactors.push({
          id: value.id,
          factor: value.factor,
          extraFactors: value.extraFactors,
          combination: value.combination,
        });
        if (value.extraFactors?.length) lastCompositeId = value.id ?? "";
        recentTasks.unshift({
          id: value.id ?? "",
          kind: TaskKind.FACTOR,
          strategy: [value.factor, ...(value.extraFactors ?? []).map(item => item.factor)].join(
            " + ",
          ),
          total: 1,
          done: 1,
          succeeded: 1,
          state: RunState.SUCCEEDED,
        });
        return {} as never;
      }
      case "getActivity":
        return create(LabActivitySchema, { active: [], recent: recentTasks }) as never;
      case "getFactorResult":
        return create(FactorResultSchema, {
          id: operation.value.id,
          evaluation: {
            factor: "price.momentum",
            combination: "ic_weighted",
            firstDay: "2025-01-02",
            lastDay: "2026-02-13",
          },
          symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
          horizons: [{ horizon: 1, days: 20, mean: 0.22, ir: 1.1 }],
          factorNames: ["price.momentum", "positions.interest_change"],
          correlations: [1.0, -0.35, -0.35, 1.0].map(value => ({ value, days: 4, samples: 24 })),
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <LabActivityProvider
      ready={true}
      currentModule="research"
      openRun={vi.fn()}
      openSweep={vi.fn()}
    >
      <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
        <FactorLab {...context} />
      </WorkspaceDraftScope>
    </LabActivityProvider>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });

  // Name the symbols once in the page's settings.
  fireEvent.click(screen.getByRole("button", { name: "设置评价条件" }));
  fireEvent.change(screen.getByPlaceholderText("选择品种或交易所…"), {
    target: { value: "SHFE," },
  });
  fireEvent.click(screen.getByRole("button", { name: "完成" }));

  // Select both factors; only then is there a combination to choose.
  expect(screen.queryByLabelText("合成方式")).toBeNull();
  fireEvent.click(within(library).getByLabelText("选择 price.momentum"));
  fireEvent.click(within(library).getByLabelText("选择 positions.interest_change"));
  fireEvent.change(screen.getByLabelText("合成方式"), { target: { value: "ic_weighted" } });

  // Batch evaluation submits a single startFactorBatch request with default params.
  const listBefore = listCount;
  fireEvent.click(screen.getByRole("button", { name: "批量评价（2）" }));
  await vi.waitFor(() => expect(listCount).toBeGreaterThan(listBefore));
  expect(startedBatches).toHaveLength(1);
  const batchEvals = startedBatches[0]!.evaluations ?? [];
  expect(batchEvals.map(item => item.factor)).toEqual([
    "price.momentum",
    "positions.interest_change",
  ]);
  expect(batchEvals.every(item => item.extraFactors?.length === 0)).toBe(true);
  expect(batchEvals[0]!.params).toEqual({ lookback: { value: { case: "integer", value: 20n } } });

  // Composite evaluation submits startFactor with extraFactors and opens the report once complete.
  fireEvent.click(screen.getByRole("button", { name: "相关性与合成（2）" }));
  expect(await screen.findByRole("table", { name: "因子相关性" })).toBeTruthy();
  expect(
    screen.getByText(/price\.momentum \+ positions\.interest_change · IC 加权合成/),
  ).toBeTruthy();
  expect(startedFactors[0]!.extraFactors).toEqual([
    { factor: "positions.interest_change", params: {} },
  ]);
  expect(startedFactors[0]!.combination).toBe("ic_weighted");

  // Returning from the composite report restores the factor library table.
  fireEvent.click(screen.getByRole("button", { name: "← 因子库" }));
  await screen.findByRole("table", { name: "因子库" });
});

test("a factor's kept evaluation reopens its report with the code it evaluated", async () => {
  const evaluation = {
    id: "price-1",
    factor: "price.momentum",
    params: { lookback: { value: { case: "integer" as const, value: 2n } } },
    firstDay: "2026-01-05",
    lastDay: "2026-02-13",
  };
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { factors: [{ id: "price.momentum" }] }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [{ id: "price-1", evaluation, evaluatedMs: 1n }],
        }) as never;
      case "getFactorResult":
        return create(FactorResultSchema, {
          id: "price-1",
          evaluation,
          code: "def momentum(close, lookback: int = 20): ...",
          pinned: { datasetVersion: "a".repeat(64) },
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "def momentum(close): ..." }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  fireEvent.click(await within(library).findByRole("button", { name: "momentum" }));
  const records = await screen.findByRole("list", { name: "评价记录" });
  fireEvent.click(within(records).getByRole("button", { name: /lookback=2/ }));
  expect(await screen.findByText(/price\.momentum · lookback=2/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "评价时的源码" }));
  expect(screen.getByText("def momentum(close, lookback: int = 20): ...")).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "代码" }));
  await screen.findByText("factors/price.py");
});

test("a factor lists the strategies that import its module and opens one's code", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [{ id: "price.momentum", inputs: ["close"] }],
          strategies: [
            { id: "momentum_rank.MomentumRank", factorModules: ["price"] },
            { id: "ma_cross.MaCross" },
          ],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {}) as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  fireEvent.click(await within(library).findByRole("button", { name: "momentum" }));
  fireEvent.click(await screen.findByText("导入本模块的策略（1）"));
  const users = await screen.findByRole("list", { name: "导入本模块的策略" });
  expect(within(users).getAllByRole("button")).toHaveLength(1);
  fireEvent.click(within(users).getByRole("button", { name: "MomentumRank" }));
  await screen.findByText("strategies/momentum_rank.py");
});

test("factor result view shows in-sample vs holdout horizons, yearly IC stability, and correlation heatmap", () => {
  render(
    <FactorResultView
      result={create(FactorResultSchema, {
        id: "multi-eval",
        evaluation: {
          factor: "price.momentum",
          firstDay: "2025-01-05",
          lastDay: "2026-02-13",
          holdoutDay: "2026-01-26",
        },
        symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
        horizons: [{ horizon: 1, days: 4, mean: 0.05, ir: 0.4 }],
        developmentHorizons: [{ horizon: 1, days: 2, mean: 0.14, ir: 0.9 }],
        holdoutHorizons: [{ horizon: 1, days: 2, mean: -0.04, ir: -0.3 }],
        days: [
          { tradingDay: "2025-06-02", ic: 0.1 },
          { tradingDay: "2025-06-03", ic: 0.2 },
          { tradingDay: "2026-01-05", ic: -0.05 },
          { tradingDay: "2026-01-06", ic: -0.15 },
        ],
        factorNames: ["price.momentum", "positions.interest_change"],
        correlations: [1.0, -0.25, -0.25, 1.0].map(value => ({ value, days: 4, samples: 24 })),
      })}
    />,
  );
  expect(screen.getByRole("table", { name: "样本内外对比" })).toBeDefined();
  const yearly = screen.getByRole("table", { name: "分年度 IC 稳定性" });
  expect(within(yearly).getByRole("rowheader", { name: "2025" })).toBeDefined();
  expect(within(yearly).getByRole("rowheader", { name: "2026" })).toBeDefined();
  expect(screen.getByText("正 IC 年份 1 / 2")).toBeDefined();

  const corr = screen.getByRole("table", { name: "因子相关性" });
  const offDiag = within(corr).getAllByText("-0.250")[0]!;
  expect(offDiag.style.background).toContain("--quote-down");
});

test("factor result view shows realism tags and net long-short KPI when cost_bps is set", () => {
  render(
    <FactorResultView
      result={create(FactorResultSchema, {
        id: "net-eval",
        evaluation: {
          factor: "price.momentum",
          firstDay: "2025-01-05",
          lastDay: "2026-02-13",
          returnMode: "open",
          winsorize: true,
          neutralize: "sector",
          volAdjust: true,
          costBps: 3,
          orthogonalizeAgainst: [{ factor: "positions.interest_change", params: {} }],
        },
        symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
        horizons: [{ horizon: 1, days: 20, mean: 0.08, ir: 0.6 }],
        days: [
          { tradingDay: "2026-01-05", groups: [1.0, 1.02], longShort: 1.02, netLongShort: 1.015 },
        ],
        groups: [
          { group: 1, meanReturn: 0.0, turnover: 0.2 },
          { group: 2, meanReturn: 0.02, turnover: 0.2 },
        ],
      })}
    />,
  );
  expect(screen.getByText(/次根开盘/)).toBeTruthy();
  expect(screen.getByText(/板块中性/)).toBeTruthy();
  expect(screen.getByText(/正交剥离 interest_change/)).toBeTruthy();
  expect(screen.getByText("扣费多空累计")).toBeTruthy();
  expect(screen.getByText("1.50%")).toBeTruthy();
});

test("cancelling a running batch factor evaluation calls cancelFactor and clears active progress", async () => {
  let activeBatchId = "";
  const cancelledIds: string[] = [];
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [{ id: "price.first" }, { id: "price.second" }],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {}) as never;
      case "startFactorBatch":
        activeBatchId = operation.value.id ?? "";
        return {} as never;
      case "cancelFactor":
        cancelledIds.push(operation.value.id ?? "");
        activeBatchId = "";
        return {} as never;
      case "getActivity":
        return create(LabActivitySchema, {
          active: activeBatchId
            ? [
                {
                  id: activeBatchId,
                  kind: TaskKind.FACTOR_BATCH,
                  total: 2,
                  done: 0,
                  running: 1,
                  queued: 1,
                  state: RunState.RUNNING,
                },
              ]
            : [],
          recent:
            !activeBatchId && cancelledIds.length
              ? [
                  {
                    id: cancelledIds[0]!,
                    kind: TaskKind.FACTOR_BATCH,
                    total: 2,
                    done: 0,
                    state: RunState.INTERRUPTED,
                  },
                ]
              : [],
        }) as never;
    }
    return new Promise(() => {}) as never;
  });
  saveFactorConditions({
    active: "SHFE",
    items: [{ name: "SHFE", fields: factorScopeOf({ ...defaultFactorForm(), symbols: "SHFE" }) }],
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <LabActivityProvider
      ready={true}
      currentModule="research"
      openRun={vi.fn()}
      openSweep={vi.fn()}
    >
      <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
        <FactorLab {...context} />
      </WorkspaceDraftScope>
    </LabActivityProvider>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  fireEvent.click(within(library).getByLabelText("选择 price.first"));
  fireEvent.click(within(library).getByLabelText("选择 price.second"));
  fireEvent.click(screen.getByRole("button", { name: "批量评价（2）" }));
  const cancel = await screen.findByRole("button", { name: "取消" });
  fireEvent.click(cancel);
  await vi.waitFor(() => expect(screen.queryByRole("button", { name: "取消" })).toBeNull());
  expect(cancelledIds).toHaveLength(1);
});

test("generating a strategy from a factor report saves it and navigates to StrategyLab with scope", async () => {
  const saved: Array<{ path?: string; content?: string; create?: boolean }> = [];
  const navigate = vi.fn();
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, { factors: [{ id: "price.momentum" }] }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [
            {
              id: "mom-1",
              evaluation: {
                factor: "price.momentum",
                firstDay: "2025-01-02",
                lastDay: "2026-02-13",
                holdoutDay: "2026-01-05",
                horizons: [5],
                groups: 3,
              },
              symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
              horizons: [{ horizon: 5, mean: 0.12 }],
            },
          ],
        }) as never;
      case "getFactorResult":
        return create(FactorResultSchema, {
          id: "mom-1",
          evaluation: { factor: "price.momentum", firstDay: "2025-01-02", lastDay: "2026-02-13" },
          symbols: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
          horizons: [{ horizon: 5, mean: 0.12 }],
        }) as never;
      case "saveSource":
        saved.push({
          path: operation.value.source?.path,
          content: operation.value.content,
          create: operation.value.create,
        });
        return {} as never;
      case "readSource":
        return create(SourceSchema, { content: "" }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate,
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  const library = await screen.findByRole("table", { name: "因子库" });
  fireEvent.click(await within(library).findByRole("button", { name: "momentum" }));
  fireEvent.click(await screen.findByRole("button", { name: "生成策略" }));
  expect(screen.getByRole("dialog", { name: "从因子生成策略" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "创建" }));
  await vi.waitFor(() => expect(saved).toHaveLength(1));
  expect(saved[0]!.path).toBe("strategies/momentum_strategy.py");
  expect(saved[0]!.create).toBe(true);
  expect(saved[0]!.content).toContain("class MomentumStrategy(Strategy):");
  expect(navigate).toHaveBeenCalledWith("workspace.lab-strategies", {
    params: {
      openStrategy: "momentum_strategy.MomentumStrategy",
      symbols: "SHFE/rb, SHFE/cu, SHFE/al",
      begin: "2025-01-02",
      end: "2026-02-13",
      holdoutDay: "2026-01-05",
    },
  });
});
