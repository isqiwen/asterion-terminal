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

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

test("the factor library ranks factors by their latest IC and opens one's code", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [
            { id: "price.momentum", inputs: ["close"] },
            { id: "price.reversal", inputs: ["close"] },
            { id: "price.volatility", inputs: ["close"] },
          ],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [
            {
              id: "b",
              evaluation: { factor: "price.reversal" },
              horizons: [{ horizon: 1, mean: 0.3 }],
            },
            {
              id: "a",
              evaluation: { factor: "price.momentum" },
              horizons: [{ horizon: 1, mean: 0.1 }],
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
  expect(screen.getAllByText("选择因子")).toHaveLength(1);
  expect(screen.queryByRole("heading", { name: "评价" })).toBeNull();
  fireEvent.click(await screen.findByRole("button", { name: "因子库" }));
  const library = await screen.findByRole("table", { name: "因子库" });
  const order = () =>
    within(library)
      .getAllByRole("row")
      .slice(1)
      .map(row => within(row).getAllByRole("cell")[0]!.textContent);
  await within(library).findByText("0.300");
  // Never evaluated goes last.
  expect(order()).toEqual(["reversal", "momentum", "volatility"]);
  fireEvent.click(within(library).getByRole("button", { name: "momentum" }));
  expect(screen.queryByRole("table", { name: "因子库" })).toBeNull();
  await screen.findByText("factors/price.py");
  expect(screen.getByRole("button", { name: "评价" })).toBeTruthy();
  expect(screen.queryByText("选择因子")).toBeNull();
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
  fireEvent.click(await screen.findByRole("button", { name: /^momentum/ }));
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
  fireEvent.click(await screen.findByRole("button", { name: /^momentum/ }));
  const users = await screen.findByRole("list", { name: "导入本模块的策略" });
  expect(within(users).getAllByRole("button")).toHaveLength(1);
  fireEvent.click(within(users).getByRole("button", { name: "MomentumRank" }));
  await screen.findByText("strategies/momentum_rank.py");
});
