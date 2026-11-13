import { render, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { ChartBarsSchema, StoredContractsSchema } from "../../src/generated/asterion/lab_pb";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { StoredContracts } from "./StoredContracts";

vi.mock("../../src/bridge/lab", () => ({
  lab: vi.fn(),
}));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));
vi.mock("@asterion/terminal/charts/PriceChart", () => ({ PriceChart: () => null }));

test("an earlier version lists its own contracts and draws its own bars", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation?.case === "listStoredContracts")
      return create(StoredContractsSchema, {
        datasetVersion: "earlier",
        contracts: [{ contract: "SHFE/rb/2026-10", daily: { days: 3 } }],
      }) as never;
    if (operation?.case === "getChartBars")
      return create(ChartBarsSchema, { datasetVersion: "earlier" }) as never;
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: {}, navigate: vi.fn() } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-browse">
      <StoredContracts version="earlier" context={context} />
    </WorkspaceDraftScope>,
  );
  await waitFor(() =>
    expect(vi.mocked(lab).mock.calls.map(([operation]) => operation?.case)).toContain(
      "getChartBars",
    ),
  );
  const versions = vi
    .mocked(lab)
    .mock.calls.map(([operation]) => [
      operation?.case,
      (operation?.value as { datasetVersion?: string }).datasetVersion,
    ]);
  expect(versions).toEqual([
    ["listStoredContracts", "earlier"],
    ["listStoredContracts", "earlier"],
    ["getChartBars", "earlier"],
  ]);
});
