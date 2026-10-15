import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("F06 daily holding spans preserve precision and keep open positions separate", async ({
  page,
}) => {
  test.setTimeout(90000);
  const prices = [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95, 90];
  const days = Array.from({ length: 20 }, (_, i) =>
    new Date(Date.UTC(2026, 8, 7 + i)).toISOString().slice(0, 10),
  )
    .filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()))
    .slice(0, prices.length);
  const seeded = await seedHistory(page.request, prices, "holding-daily", {
    tablesOnly: true,
    dailyPrices: true,
    dailyDays: days,
  });
  await rpc(page.request, "data.dataset.daily", {
    contract_id: "SHFE/rb/2026-10",
    daily: seeded.daily,
    begin_day: days[0],
    end_day: days.at(-1)!,
  });
  await rpc(page.request, "backtest.submit", {
    id: "holding-baseline",
    strategies: [{ quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } }],
    holdout_from: "",
    walk_forward: null,
    deposit: "100000",
    max_order_quantity: "1",
    max_gross_quantity: "1",
    max_working_orders: "4",
    contracts: [
      {
        venue: "SHFE",
        symbol: "rb2610",
        slippage_ticks: 0,
        cost_schedule: [
          {
            effective_from: "1970-01-01",
            source: "isolated explicit test cost",
            values: {
              margin_per_lot: "100",
              margin_rate: "0",
              open_fee: "1",
              close_today_fee: "1",
              close_yesterday_fee: "1",
              open_fee_rate: "0",
              close_today_fee_rate: "0",
              close_yesterday_fee_rate: "0",
            },
          },
        ],
      },
    ],
  });
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).task_service.tasks;
  await expect
    .poll(
      async () => (await tasks()).find((t: { id: string }) => t.id === "holding-baseline")?.state,
    )
    .toBe("succeeded");
  const original = (await rpc(page.request, "task.result.read", { id: "holding-baseline" }))
    .task_result;
  expect(original.result.fill_days.length).toBe(original.result.account.fills.length);
  expect(original.trades.closed[0].holding).toEqual({
    opened_day: "2026-09-10",
    ended_day: "2026-09-14",
    calendar_days: 4,
    settlement_days: 2,
  });
  expect(original.trades.holding_summary.known).toBe(original.trades.count);
  expect(original.trades.holding_summary.unknown).toBe(0);
  expect(original.trades.open).toHaveLength(1);
  expect(original.trades.open[0].as_of_day).toBe("2026-09-23");
  expect(original.trades.open[0].holding.calendar_days).toBe(0);
  const ids = (await tasks()).map((t: { id: string }) => t.id);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "holding-baseline" })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const stats = workspace.getByRole("region", { name: "按笔统计", exact: true });
  await stats.getByText("持有期（交易日期口径）", { exact: true }).click();
  const closed = stats.getByRole("table", { name: "已平仓持有期", exact: true });
  await expect(closed).toContainText("2026-09-10");
  await expect(closed).toContainText("2026-09-14");
  await expect(stats.getByRole("table", { name: "未平仓持有跨度", exact: true })).toContainText(
    "2026-09-23",
  );
  await expect(stats).toContainText("不代表持有0分钟");
  const again = (await rpc(page.request, "task.result.read", { id: "holding-baseline" }))
    .task_result;
  expect(again).toEqual(original);
  expect((await tasks()).map((t: { id: string }) => t.id)).toEqual(ids);
});
