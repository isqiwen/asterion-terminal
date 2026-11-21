import { fireEvent, render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import {
  FactorResultSchema,
  FactorResultsSchema,
  SourceSchema,
  StrategiesSchema,
} from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { FactorLab } from "./FactorLab";
import { FactorResultView } from "./FactorResultView";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, string | number>) =>
    values ? key.replace(/\{(\w+)\}/g, (_, name: string) => String(values[name] ?? "")) : key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

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

test("opens the first factor by default, and the factor library ranks, filters, and opens a factor", async () => {
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
  // Opens the first factor (alphabetical: interest_change) with code and config ready.
  await screen.findByText("factors/positions.py");
  expect(screen.getByRole("button", { name: "评价" })).toBeTruthy();

  // Switching to the factor library hides the single-factor config and ranks by IC
  // while keeping composite evaluations out of single-factor rows.
  fireEvent.click(screen.getByRole("button", { name: /^因子库/ }));
  expect(screen.queryByRole("heading", { name: "评价" })).toBeNull();
  const library = await screen.findByRole("table", { name: "因子库" });
  const order = () =>
    within(library)
      .getAllByRole("row")
      .slice(1)
      .map(row => within(row).getByRole("rowheader").textContent);
  await within(library).findByText("0.300");
  expect(within(library).getByText("0.180")).toBeTruthy();
  expect(within(library).getByText("50.00%")).toBeTruthy();
  expect(within(library).getByText("20.00%")).toBeTruthy();
  expect(within(library).getAllByText("2")).toHaveLength(2);
  // Never evaluated goes last.
  expect(order()).toEqual(["reversal", "momentum", "interest_change"]);

  // Sorting by holdout IC updates row order, aria-sort, and status announcement.
  fireEvent.click(within(library).getByRole("button", { name: "留出期 IC" }));
  expect(order()).toEqual(["momentum", "reversal", "interest_change"]);
  expect(
    within(library)
      .getByRole("columnheader", { name: /留出期 IC/ })
      .getAttribute("aria-sort"),
  ).toBe("descending");
  expect(screen.getByRole("status").textContent).toBe("已按 留出期 IC降序排序");

  // Sidebar search syncs with the factor library table (including bar fields).
  fireEvent.change(screen.getByLabelText("搜索因子"), { target: { value: "close" } });
  expect(order()).toEqual(["momentum", "reversal"]);

  // Clicking a factor row opens its code and restores the evaluation panel.
  fireEvent.click(within(library).getByRole("button", { name: "momentum" }));
  expect(screen.queryByRole("table", { name: "因子库" })).toBeNull();
  await screen.findByText("factors/price.py");
  expect(screen.getByRole("button", { name: "评价" })).toBeTruthy();
});

test("factor library runs batch evaluation and multi-factor composite evaluation from selected rows", async () => {
  const evaluated: Array<{
    factor?: string;
    extraFactors?: Array<{ factor?: string }>;
    combination?: string;
    params?: Record<string, unknown>;
  }> = [];
  let listCount = 0;
  let lastCompositeId = "";
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
      case "evaluateFactor": {
        const value = operation.value;
        evaluated.push({
          factor: value.factor,
          extraFactors: value.extraFactors,
          combination: value.combination,
          params: value.params,
        });
        if (value.extraFactors?.length) lastCompositeId = value.id ?? "";
        return create(FactorResultSchema, { id: value.id }) as never;
      }
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
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  await screen.findByText("factors/positions.py");
  fireEvent.click(screen.getByRole("button", { name: /^因子库/ }));
  const library = await screen.findByRole("table", { name: "因子库" });

  // Enter symbols and choose IC-weighted combination in the library side panel.
  fireEvent.change(screen.getByPlaceholderText("选择品种或交易所…"), {
    target: { value: "SHFE," },
  });
  fireEvent.change(screen.getByLabelText("合成方式"), { target: { value: "ic_weighted" } });

  // Select both factors.
  fireEvent.click(within(library).getByLabelText("选择 price.momentum"));
  fireEvent.click(within(library).getByLabelText("选择 positions.interest_change"));

  // Batch evaluation runs each selected factor individually with default params.
  const listBefore = listCount;
  fireEvent.click(screen.getByRole("button", { name: "批量评价（2）" }));
  await vi.waitFor(() => expect(listCount).toBeGreaterThan(listBefore));
  expect(evaluated).toHaveLength(2);
  expect(evaluated.map(item => item.factor)).toEqual([
    "price.momentum",
    "positions.interest_change",
  ]);
  expect(evaluated.every(item => item.extraFactors?.length === 0)).toBe(true);
  expect(evaluated[0]!.params).toEqual({ lookback: { value: { case: "integer", value: 20n } } });

  // Composite evaluation submits both factors together and opens the correlation report.
  fireEvent.click(screen.getByRole("button", { name: "相关性与合成（2）" }));
  expect(await screen.findByRole("table", { name: "因子相关性" })).toBeTruthy();
  expect(
    screen.getByText(/price\.momentum \+ positions\.interest_change · IC 加权合成/),
  ).toBeTruthy();
  expect(evaluated[2]!.extraFactors).toEqual([{ factor: "positions.interest_change", params: {} }]);
  expect(evaluated[2]!.combination).toBe("ic_weighted");

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
  const sidebar = screen.getByRole("complementary", { name: "因子" });
  fireEvent.click(await within(sidebar).findByRole("button", { name: /^momentum/ }));
  const records = await screen.findByRole("list", { name: "评价记录" });
  fireEvent.click(within(records).getByRole("button", { name: /lookback=2/ }));
  expect(await screen.findByText(/price\.momentum · lookback=2/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "评价时的源码" }));
  expect(screen.getByText("def momentum(close, lookback: int = 20): ...")).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "← 源码" }));
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
  const sidebar = screen.getByRole("complementary", { name: "因子" });
  fireEvent.click(await within(sidebar).findByRole("button", { name: /^momentum/ }));
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
