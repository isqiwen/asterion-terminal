import { rpc, seedHistory } from "./dataset-fixture";
import { checkSnapshot } from "./snapshot-schema";
import { test, expect } from "./test";

// The real data service with the bundled data source: what it offers reaches
// the page without a single request to the vendor. Other specs seed SHFE's
// daily table, so what is asserted here is what no spec stores.
test("the market tables page lists what the data source offers", async ({ page }) => {
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
  // Without a credential nothing can be asked of the source.
  await expect(tables.getByRole("button", { name: "开始更新", exact: true })).toBeDisabled();
  // What the core answers fits the Terminal's types.
  const state = await rpc(page.request, "data.tables.coverage", {});
  const held = state.table_coverage;
  checkSnapshot("market table coverage", state);
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
test("a factor on the market-wide table leads to the update page", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await page.getByRole("button", { name: "全市场数据表", exact: true }).click();
  await expect(page.getByRole("button", { name: "开始分析", exact: true })).toBeDisabled();
  await page.getByRole("button", { name: "更新全市场数据", exact: true }).click();
  await expect(page.getByRole("region", { name: "全市场数据", exact: true })).toBeVisible();
});

// The factor command asks the daily table of each range by itself: instruments
// of an exchange that began inside the days asked for are read from that day.
test("a factor reads instruments of the daily table over the range given for each", async ({
  page,
}) => {
  test.setTimeout(90000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // Forty weekdays of three contracts in the daily table.
  const days: string[] = [];
  for (let at = Date.UTC(2026, 7, 17); days.length < 40; at += 86400000)
    if (new Date(at).getUTCDay() % 6 !== 0) days.push(new Date(at).toISOString().slice(0, 10));
  let daily = { snapshot: "", daily_source: "", calendar_source: "" };
  for (const product of ["rb", "hc", "al"])
    daily = (
      await seedHistory(page.request, [100], `tables-${product}`, {
        product,
        minuteDays: [days[0]],
        dailyDays: days,
      })
    ).daily;
  await rpc(page.request, "factor.submit", {
    id: "tables-factor",
    series: {
      kind: "tables",
      ...daily,
      end_day: days[39],
      ranges: [
        { begin_day: days[0], instruments: ["SHFE/rb/2026-10", "SHFE/hc/2026-10"] },
        { begin_day: days[5], instruments: ["SHFE/al/2026-10"] },
      ],
    },
    factor: "momentum",
    lookbacks: [2],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).task_service.tasks.find(
          (task: { id: string }) => task.id === "tables-factor",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const evidence = (await rpc(page.request, "task.result", { id: "tables-factor" })).task_result;
  expect(
    evidence.experiment.series.map(
      (series: { data: { instrument: string; begin_day: string; count: number } }) => [
        series.data.instrument,
        series.data.begin_day,
        series.data.count,
      ],
    ),
  ).toEqual([
    ["SHFE/rb/2026-10", days[0], 40],
    ["SHFE/hc/2026-10", days[0], 40],
    ["SHFE/al/2026-10", days[5], 35],
  ]);
  // A contract that cannot be read is named with the others that cannot.
  const refused = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "factor.submit",
      params: {
        id: "tables-unreadable",
        series: {
          kind: "tables",
          ...daily,
          end_day: days[39],
          ranges: [
            {
              begin_day: days[0],
              instruments: ["SHFE/rb/2026-10", "SHFE/zn/2026-10", "SHFE/ni/2026-10"],
            },
          ],
        },
        factor: "momentum",
        lookbacks: [2],
        horizon: 1,
        evaluation: { mode: "full_sample" },
      },
    },
  });
  expect((await refused.json()).error.message).toBe(
    "instruments cannot be read: SHFE/zn/2026-10: it has no closes in the range; " +
      "SHFE/ni/2026-10: it has no closes in the range",
  );
});
