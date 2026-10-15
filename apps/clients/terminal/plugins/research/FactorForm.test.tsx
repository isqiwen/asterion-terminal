import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { Snapshot, TableCoverage, TableProduct } from "../../src/bridge/client";
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
// Three products listed throughout, one whose first contract came inside the
// range the test asks for, and one whose last contract left before it. A
// second exchange lists its first contract inside the range.
const products: TableProduct[] = [
  ...["al", "cu", "rb", "op", "wr"].map(product => ({
    exchange: "SHFE",
    product,
    contracts: 12,
    first_listed: product === "op" ? "2026-09-25" : "1995-04-17",
    last_delisted: product === "wr" ? "2026-09-15" : "2027-10-15",
  })),
  {
    exchange: "GFEX",
    product: "si",
    contracts: 12,
    first_listed: "2026-09-28",
    last_delisted: "2027-10-15",
  },
];

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
  // Everything listed at some time in the range joins what is already chosen.
  fireEvent.click(screen.getByRole("button", { name: "移除 SHFE/al" }));
  fireEvent.click(screen.getByRole("button", { name: "添加范围内有合约在市的全部品种" }));
  expect(screen.getByRole("list", { name: "已选的品种和合约" }).textContent).toBe(
    "SHFE/rb×SHFE/cu×SHFE/al×SHFE/op×GFEX/si×",
  );
  // An exchange that began inside the range is read from the day it began; a
  // product that began inside it in an older exchange needs no other range.
  expect(screen.getByRole("status").textContent).toBe(
    "交易所在起始日期之后才有合约上市的，它的品种从第一个上市日起读取：GFEX 2026-09-28。",
  );
  fireEvent.click(screen.getByLabelText("启用历史流动性筛选"));
  fireEvent.change(screen.getByLabelText("此前连续观测数"), { target: { value: "3" } });
  fireEvent.change(screen.getByLabelText("上一观测最低成交量（手）"), {
    target: { value: "9007199254740993" },
  });
  fireEvent.change(screen.getByLabelText("上一观测最低持仓量（手）"), { target: { value: "100" } });
  fireEvent.click(start);
  await waitFor(() => expect(run).toHaveBeenCalled());
  expect(run).toHaveBeenCalledWith("factor.submit", {
    id: expect.stringMatching(/^factor-/),
    series: {
      kind: "tables",
      snapshot: coverage.snapshot,
      daily_source: "tushare.fut_daily.v3",
      calendar_source: "tushare.trade_cal.v1",
      end_day: "2026-10-09",
      ranges: [
        { begin_day: "2026-09-21", instruments: ["SHFE/rb", "SHFE/cu", "SHFE/al", "SHFE/op"] },
        { begin_day: "2026-09-28", instruments: ["GFEX/si"] },
      ],
    },
    factor: "term_structure",
    lookbacks: [5],
    horizon: 1,
    evaluation: { mode: "full_sample" },
    liquidity: {
      minimum_history: 3,
      minimum_volume: "9007199254740993",
      minimum_open_interest: "100",
    },
  });
});

test("with no daily table stored the form says so and has nothing to start", async () => {
  const snapshot = readySnapshot();
  // The data source offers the tables; nothing of them is stored yet.
  const query: TerminalContext["query"] = async () => ({
    ...snapshot,
    table_coverage: { snapshot: "", sources: coverage.sources, rows: [] },
    table_products: [],
  });
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="test">
      <TableFactorForm
        snapshot={snapshot}
        query={query}
        busy={false}
        navigate={() => {}}
        run={async () => true}
      />
    </WorkspaceDraftScope>,
  );
  expect((await screen.findByRole("status")).textContent).toBe("还没有入库的全市场日线。");
  expect(screen.queryByLabelText("品种或合约")).toBeNull();
  expect((screen.getByRole("button", { name: "开始分析" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
});

test("what is chosen and submitted belongs to the services it was made on", async () => {
  const query: TerminalContext["query"] = async method =>
    method === "data.tables.products"
      ? { ...readySnapshot(), table_products: products }
      : { ...readySnapshot(), table_coverage: coverage };
  const drafts = createWorkspaceDrafts();
  const form = (snapshot: Snapshot) => (
    <WorkspaceDraftScope store={drafts} namespace="test">
      <TableFactorForm
        snapshot={snapshot}
        query={query}
        busy={false}
        navigate={() => {}}
        run={async () => true}
      />
    </WorkspaceDraftScope>
  );
  const view = render(form(readySnapshot()));
  fireEvent.change(await screen.findByLabelText("品种或合约"), { target: { value: "SHFE/rb" } });
  fireEvent.click(screen.getByRole("button", { name: "添加" }));
  fireEvent.change(screen.getByLabelText("起始日期"), { target: { value: "2026-09-21" } });
  fireEvent.change(screen.getByLabelText("结束日期"), { target: { value: "2026-10-09" } });
  fireEvent.click(screen.getByRole("button", { name: "开始分析" }));
  expect(await screen.findByText("任务已提交，可在任务中心查看进度。")).toBeTruthy();
  // Another data service: its store is read anew, and neither the choice nor
  // the submission made on the first carries over.
  const other = readySnapshot();
  other.data = { ...other.data!, connection_id: "other" };
  view.rerender(form(other));
  await screen.findByLabelText("品种或合约");
  expect(screen.getByRole("list", { name: "已选的品种和合约" }).textContent).toBe("");
  expect(screen.queryByText("任务已提交，可在任务中心查看进度。")).toBeNull();
});
