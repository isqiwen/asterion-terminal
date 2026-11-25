import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
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
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
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

test("stored contracts can be found by short code, identity or product name", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation?.case === "listStoredContracts")
      return create(StoredContractsSchema, {
        contracts: [
          { contract: "SHFE/rb/2026-10", daily: { days: 3 } },
          { contract: "SHFE/au/2025-12", daily: { days: 263 } },
        ],
      }) as never;
    if (operation?.case === "getChartBars") return create(ChartBarsSchema) as never;
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: {}, navigate: vi.fn() } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-browse">
      <StoredContracts version="" context={context} />
    </WorkspaceDraftScope>,
  );
  const search = await screen.findByRole("searchbox", { name: "搜索已采集合约" });
  for (const value of [" Au2512 ", "SHFE/au/2025-12", "沪金"]) {
    fireEvent.change(search, { target: { value } });
    const contracts = within(screen.getByRole("group", { name: "已采集合约" }));
    expect(contracts.getAllByRole("button")).toHaveLength(1);
    expect(contracts.getByRole("button").title).toBe("SHFE/au/2025-12");
    expect(contracts.getByRole("button").getAttribute("aria-pressed")).toBe("true");
  }
});

test("the list and chart header show readable contract names and exchange metadata instead of raw paths", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    if (operation?.case === "listStoredContracts")
      return create(StoredContractsSchema, {
        contracts: [
          {
            contract: "CFFEX/ic/2026-11",
            name: "IC2611",
            daily: { days: 58 },
            minutes: { days: 30 },
          },
          { contract: "SHFE/au/2025-12", daily: { days: 263 } },
        ],
      }) as never;
    if (operation?.case === "getChartBars") return create(ChartBarsSchema) as never;
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: {}, navigate: vi.fn() } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-browse">
      <StoredContracts version="" context={context} />
    </WorkspaceDraftScope>,
  );
  const contracts = within(await screen.findByRole("group", { name: "已采集合约" }));
  const buttons = contracts.getAllByRole("button");
  expect(buttons[0].textContent).toContain("IC2611");
  expect(buttons[0].textContent).toContain("CFFEX");
  expect(buttons[0].textContent).not.toContain("CFFEX/ic/2026-11");
  expect(buttons[0].textContent).not.toContain("ic2611");
  expect(buttons[1].textContent).toContain("沪金2512");
  expect(buttons[1].textContent).toContain("au2512 · SHFE");

  const chart = screen.getByRole("region", { name: "合约历史 K 线" });
  expect(chart.textContent).toContain("IC2611");
  expect(chart.textContent).toContain("CFFEX");
  expect(chart.textContent).toContain("日线 58 天 · 分钟线 30 天");
  expect(chart.textContent).not.toContain("CFFEX/ic/2026-11");
});
