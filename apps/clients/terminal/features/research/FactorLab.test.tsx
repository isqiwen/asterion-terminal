import { fireEvent, render, screen } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import {
  FactorResultSchema,
  FactorResultsSchema,
  StrategiesSchema,
} from "../../src/generated/asterion/lab_pb";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { FactorLab } from "./FactorLab";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

test("a kept evaluation reopens with the code it evaluated", async () => {
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
        return create(StrategiesSchema, {}) as never;
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
    }
    return new Promise(() => {}) as never;
  });
  const context = {
    snapshot: { research_directory: "/research" },
    navigate: vi.fn(),
  } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "price-1" }));
  expect(await screen.findByText("def momentum(close, lookback: int = 20): ...")).toBeTruthy();
  expect(screen.getByText(/price\.momentum · lookback=2/)).toBeTruthy();
});
