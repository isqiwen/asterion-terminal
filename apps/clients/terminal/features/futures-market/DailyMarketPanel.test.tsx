import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { Snapshot, TableCoverage, TableListedContracts } from "../../src/bridge/client";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { DailyMarketPanel } from "./DailyMarketPanel";
import { feature } from "./feature";

registerTerminalFeatures([feature]);
vi.mock("./ContractHistory", () => ({
  ContractHistory: ({
    historyContractId,
    dailyTable,
  }: {
    historyContractId: string;
    dailyTable: unknown;
  }) => (
    <output aria-label="chart">
      {JSON.stringify({ contract: historyContractId, daily: dailyTable })}
    </output>
  ),
}));
const coverage: TableCoverage = {
  snapshot: "a".repeat(64),
  sources: ["daily", "calendar", "contracts"].map(name => ({
    table: `futures.${name}`,
    schema: 1,
    source: `fixture.${name}.v1`,
    plugin_id: "fixture",
    venues: ["SHFE"],
  })),
  rows: [
    {
      table: "futures.daily",
      schema: 1,
      source: "fixture.daily.v1",
      exchange: "SHFE",
      partitions: 1,
      rows: 2,
      first_period: "2026-03-02",
      last_period: "2026-03-02",
      acquired_at_ns: "1000",
      missing_days: 0,
      missing_examples: [],
    },
  ],
};
const items = (product: string): TableListedContracts["items"] =>
  ["05", "10"].map(month => ({
    contract_id: `SHFE/${product}/2026-${month}`,
    name: `${product}${month}`,
    list_date: "2025-01-01",
    delist_date: `2026-${month}-15`,
  }));
const answer = (snapshot: Snapshot, method: string): Snapshot => ({
  ...snapshot,
  ...(method === "data.tables.coverage" ? { table_coverage: coverage } : {}),
  ...(method === "data.tables.products"
    ? {
        table_products: ["cu", "rb"].map(product => ({
          exchange: "SHFE",
          product,
          contracts: 2,
          first_listed: "2025-01-01",
          last_delisted: "2026-10-15",
        })),
      }
    : {}),
});
const page = (context: TerminalContext, store = createWorkspaceDrafts()) => (
  <WorkspaceDraftScope store={store} namespace="market">
    <DailyMarketPanel context={context} />
  </WorkspaceDraftScope>
);
const contextFor = (snapshot: Snapshot, query: TerminalContext["query"]): TerminalContext => ({
  snapshot,
  query,
  busy: false,
  error: "",
  trade: async () => {},
  navigate: vi.fn(),
  openSettings: vi.fn(),
});

test("daily-only contracts select a month and pin the chart to the catalog snapshot", async () => {
  const snapshot = readySnapshot();
  const query = vi.fn<TerminalContext["query"]>(async (...[method, params]) => ({
    ...answer(snapshot, method),
    ...(method === "data.tables.listed_contracts"
      ? {
          table_listed_contracts: {
            query: params as TableListedContracts["query"],
            items: items(params.product as string),
          },
        }
      : {}),
  }));
  render(page(contextFor(snapshot, query)));
  await screen.findByLabelText("chart");
  fireEvent.change(screen.getByLabelText("品种"), { target: { value: "rb" } });
  await waitFor(() =>
    expect(screen.getByLabelText("chart").textContent).toContain("SHFE/rb/2026-05"),
  );
  fireEvent.change(screen.getByLabelText("合约月份"), { target: { value: "SHFE/rb/2026-10" } });
  expect(JSON.parse(screen.getByLabelText("chart").textContent!)).toEqual({
    contract: "SHFE/rb/2026-10",
    daily: {
      snapshot: coverage.snapshot,
      daily_source: "fixture.daily.v1",
      calendar_source: "fixture.calendar.v1",
      contracts_source: "fixture.contracts.v1",
    },
  });
  expect(query.mock.calls.some(([method]) => method === "data.datasets")).toBe(false);
  expect(screen.getByText(/合约目录不代表行情数据完整/)).toBeTruthy();
});

test("a delayed catalog from the previous data service cannot select a contract", async () => {
  const first = readySnapshot();
  let deliver: (value: Snapshot) => void = () => {
    throw new Error("query not started");
  };
  const query = vi.fn<TerminalContext["query"]>(async (...[method]) => {
    if (method === "data.tables.listed_contracts")
      return new Promise(resolve => {
        deliver = resolve;
      });
    return answer(first, method);
  });
  const store = createWorkspaceDrafts();
  const view = render(page(contextFor(first, query), store));
  await waitFor(() =>
    expect(query).toHaveBeenCalledWith("data.tables.listed_contracts", expect.anything()),
  );
  const second = readySnapshot();
  second.data!.connection_id = "another-service";
  const newQuery: TerminalContext["query"] = async () => ({
    ...second,
    table_coverage: { ...coverage, rows: [] },
  });
  view.rerender(page(contextFor(second, newQuery), store));
  deliver({
    ...first,
    table_listed_contracts: {
      query: {
        snapshot: coverage.snapshot,
        contracts_source: "fixture.contracts.v1",
        exchange: "SHFE",
        product: "cu",
      },
      items: items("cu"),
    },
  });
  await screen.findByText("请先在数据工作区更新日线、交易日历和合约表。");
  expect(screen.queryByLabelText("chart")).toBeNull();
  expect((screen.getByLabelText("合约月份") as HTMLSelectElement).disabled).toBe(true);
});
