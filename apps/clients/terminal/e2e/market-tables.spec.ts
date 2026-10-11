import { rpc } from "./dataset-fixture";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect } from "./test";

// The real data service with the bundled data source: what it offers reaches
// the page without a single request to the vendor.
test("the market tables page lists what the data source offers before anything is stored", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "全市场数据", exact: true }).click();
  const tables = page.getByRole("region", { name: "全市场数据", exact: true });
  const coverage = tables.getByRole("table", { name: "全市场数据覆盖" });
  // Six tables of six exchanges under one heading row; CFFEX has no warehouse
  // receipts, so that table has five.
  await expect(coverage.getByRole("row")).toHaveCount(36);
  await expect(coverage.getByRole("row").filter({ hasText: "仓单日报" })).toHaveCount(5);
  await expect(
    coverage.getByRole("row").filter({ hasText: "日线" }).filter({ hasText: "GFEX" }),
  ).toContainText("尚未下载");
  // Nothing is stored yet, so there is no day to continue from; and without a
  // credential nothing can be asked of the source.
  await expect(tables.getByLabel("起始日期", { exact: true })).toHaveAttribute("required", "");
  await expect(tables.getByRole("button", { name: "开始更新", exact: true })).toBeDisabled();
  // What the core answers fits the Terminal's types.
  const state = await rpc(page.request, "data.tables.coverage", {});
  const held = state.table_coverage;
  checkSnapshot("market table coverage", state);
  expect(held.snapshot).toBe("");
  expect(held.sources.map((source: { table: string }) => source.table)).toEqual([
    "futures.contracts",
    "futures.calendar",
    "futures.daily",
    "futures.settlement",
    "futures.receipts",
    "futures.rankings",
  ]);
});

// The research plugin reads the same store through the real core.
test("a factor on the market-wide table waits for stored data and leads to the update page", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await page.getByRole("button", { name: "全市场数据表", exact: true }).click();
  await expect(page.getByText("还没有入库的全市场日线。", { exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "开始分析", exact: true })).toBeDisabled();
  await page.getByRole("button", { name: "更新全市场数据", exact: true }).click();
  await expect(page.getByRole("region", { name: "全市场数据", exact: true })).toBeVisible();
});
