import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { TableCoverage, TableProduct } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { TableFactorForm } from "./FactorForm";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);

const provider = "asterion.data.tushare";
const source = (table: string, id: string) => ({
  table,
  schema: 1,
  source: id,
  plugin_id: provider,
  venues: ["SHFE"],
});
// One data source whose daily table holds two weeks of one exchange.
const coverage: TableCoverage = {
  snapshot: "c".repeat(64),
  sources: [
    source("futures.contracts", "tushare.fut_basic.v1"),
    source("futures.calendar", "tushare.trade_cal.v1"),
    source("futures.daily", "tushare.fut_daily.v3"),
  ],
  rows: [
    {
      table: "futures.daily",
      schema: 1,
      source: "tushare.fut_daily.v3",
      exchange: "SHFE",
      partitions: 10,
      rows: 2380,
      first_period: "2026-09-21",
      last_period: "2026-10-09",
      acquired_at_ns: "1791717000000000000",
      missing_days: 0,
      missing_examples: [],
    },
  ],
};
// Three products listed throughout, and one whose first contract came later.
const products: TableProduct[] = ["al", "cu", "rb", "op"].map(product => ({
  exchange: "SHFE",
  product,
  contracts: 12,
  first_listed: product === "op" ? "2026-09-25" : "1995-04-17",
  last_delisted: "2027-10-15",
}));

test("instruments of the market-wide table are submitted together under the store's snapshot", async () => {
  const asked: [string, Record<string, unknown>][] = [];
  const snapshot = readySnapshot();
  const query: TerminalContext["query"] = async (method, params) => {
    asked.push([method, params]);
    return method === "data.tables.products"
      ? { ...snapshot, table_products: products }
      : { ...snapshot, table_coverage: coverage };
  };
  const run = vi.fn(async () => true);
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="test">
      <TableFactorForm
        snapshot={snapshot}
        query={query}
        busy={false}
        navigate={() => {}}
        run={run}
      />
    </WorkspaceDraftScope>,
  );
  const entry = await screen.findByLabelText("品种或合约");
  expect(asked).toEqual([
    ["data.tables.coverage", {}],
    [
      "data.tables.products",
      { snapshot: coverage.snapshot, contracts_source: "tushare.fut_basic.v1" },
    ],
  ]);
  expect(document.body.textContent).toContain("已入库的日线：SHFE 2026-09-21 — 2026-10-09");
  const add = (name: string) => {
    fireEvent.change(entry, { target: { value: name } });
    fireEvent.click(screen.getByRole("button", { name: "添加" }));
  };
  const start = screen.getByRole("button", { name: "开始分析" }) as HTMLButtonElement;
  add("SHFE/rb");
  add("SHFE/cu");
  // Two are neither one series nor a cross-section; a name that is neither a
  // product nor a contract is not taken at all.
  add("rb2610");
  expect(screen.getByRole("list", { name: "已选的品种和合约" }).textContent).toBe(
    "SHFE/rb×SHFE/cu×",
  );
  expect(screen.getByRole("alert").textContent).toBe(
    "截面比较至少需要 3 个品种或合约；只分析一个时请只保留一个。",
  );
  add("SHFE/al");
  fireEvent.change(screen.getByLabelText("起始日期"), { target: { value: "2026-09-21" } });
  expect(start.disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("结束日期"), { target: { value: "2026-10-09" } });
  expect(start.disabled).toBe(false);
  // The term structure is of a product's months: a single contract has none.
  add("SHFE/rb/2026-10");
  fireEvent.change(screen.getByLabelText("因子"), { target: { value: "term_structure" } });
  expect(start.disabled).toBe(true);
  fireEvent.click(screen.getByRole("button", { name: "移除 SHFE/rb/2026-10" }));
  expect(start.disabled).toBe(false);
  // Everything listed from the first day to the last joins what is already chosen.
  fireEvent.click(screen.getByRole("button", { name: "移除 SHFE/al" }));
  fireEvent.click(screen.getByRole("button", { name: "添加范围内上市的全部品种" }));
  expect(screen.getByRole("list", { name: "已选的品种和合约" }).textContent).toBe(
    "SHFE/rb×SHFE/cu×SHFE/al×",
  );
  fireEvent.click(start);
  await waitFor(() => expect(run).toHaveBeenCalled());
  expect(run).toHaveBeenCalledWith("factor.submit", {
    id: expect.stringMatching(/^factor-/),
    series: {
      kind: "tables",
      snapshot: coverage.snapshot,
      daily_source: "tushare.fut_daily.v3",
      calendar_source: "tushare.trade_cal.v1",
      begin_day: "2026-09-21",
      end_day: "2026-10-09",
      instruments: ["SHFE/rb", "SHFE/cu", "SHFE/al"],
    },
    factor: "term_structure",
    lookbacks: [5],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
});
