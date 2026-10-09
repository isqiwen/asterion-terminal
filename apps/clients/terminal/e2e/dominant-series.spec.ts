import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";

test("a product's months backtest as its dominant series and roll at real prices", async ({
  page,
}) => {
  test.setTimeout(60000);
  await page.goto("/");
  // Startup starts the data service; seeding stops it, so let startup finish.
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // rb2610 holds the open interest until rb2701 appears with more of it, at
  // twice the price: the roll falls on 2026-09-25.
  await seedHistory(
    page.request,
    [100, 100, 100, 100, 104, 103, 106, 105, 107, 108, 109, 110, 110, 110, 110],
    "near",
    {
      minuteDays: ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"],
      settlement: 110,
    },
  );
  await seedHistory(page.request, [216, 218, 220, 222, 224, 223, 225, 226, 227], "far", {
    keep: true,
    month: "2027-01",
    minuteDays: ["2026-09-24", "2026-09-25", "2026-09-28"],
    openInterest: 200,
    settlement: 220,
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  await workspace.getByRole("button", { name: "主力连续", exact: true }).click();
  const product = workspace.getByLabel("品种", { exact: true });
  // Daily downloads form a series of their own.
  await expect(product.locator("option")).toHaveCount(3);
  for (const name of [
    "选择品种",
    "SHFE/rb · 日线 · tushare.fut_daily · 2 个月份",
    "SHFE/rb · 1 分钟 · tushare.ft_mins · 2 个月份",
  ])
    await expect(product.getByRole("option", { name, exact: true })).toHaveCount(1);
  await product.selectOption({ label: "SHFE/rb · 1 分钟 · tushare.ft_mins · 2 个月份" });
  const series = workspace.getByRole("region", { name: "主力连续", exact: true });
  await series.getByLabel("最小变动价位").fill("1");
  await series.getByLabel("合约乘数").fill("10");
  await series.getByRole("button", { name: "使用主力连续（2 个月份）", exact: true }).click();
  const selected = workspace.getByRole("list", { name: "已选合约" });
  await expect(selected).toContainText("SHFE · rb 主力连续");
  await expect(selected).toContainText("18 根 · 2 个月份");
  // The series begins on the second day; each month is listed from the day
  // it becomes dominant, with the factor that scales its prices for signals.
  await selected.getByText("换月日程（1 次换月）", { exact: true }).click();
  const rolls = selected.getByRole("table", { name: "SHFE · rb 主力连续 换月日程" });
  await expect(rolls.locator("tbody tr")).toHaveText([
    /2026-09-22\s*rb2610\s*2/,
    /2026-09-25\s*rb2701\s*1/,
  ]);
  const state = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("dominant series selection", state);
  expect(state.dataset_series).toHaveLength(1);
  expect(state.datasets.map((item: { symbol: string }) => item.symbol)).toEqual([
    "rb2610",
    "rb2701",
  ]);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  // One set of costs serves every month of the series.
  await expect(
    workspace.getByRole("region", { name: "SHFE · rb 主力连续 保证金与手续费" }),
  ).toBeVisible();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "1"],
    ["总持仓量上限", "1"],
    ["在途委托数上限", "2"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(
    workspace.getByText("任务已提交，可在任务中心查看进度。", { exact: true }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "返回回测记录", exact: true }).click();
  const row = workspace.getByRole("region", { name: "研究任务", exact: true }).locator("tbody tr");
  await expect(row.first().getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  // Bought rb2610 at 105, sold it at 110 on the roll day, then bought rb2701
  // at 223 and settled at 220. The day's last bar asks to go flat; a target
  // survives the day end, so the first bar of 2026-09-28 sells at 225:
  // 50 + 20 - 12 in fees. The gross limit of one lot was never exceeded.
  const task = (await rpc(page.request, "runtime.snapshot")).task_service.tasks.at(-1);
  const result = (await rpc(page.request, "task.result", { id: task.id })).task_result.result;
  expect(result.account.fills.map((fill: { price: string }) => fill.price)).toEqual([
    "105",
    "110",
    "223",
    "225",
  ]);
  expect(result.account.equity).toBe("10058");
  expect(result.settlements).toHaveLength(5);
  await row.first().getByRole("button", { name: "查看结果", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "回测结果", exact: true }).getByRole("img", {
      name: "权益曲线",
    }),
  ).toBeVisible();
  // Reattaching the same store keeps the series; switching stores must not
  // retain month identities from the original archive.
  const originalService = state.task_service!.service;
  const reattached = await rpc(page.request, "node.data_tasks.attach", {
    id: "local",
    service: originalService,
  });
  expect(reattached.dataset_series).toEqual(state.dataset_series);
  expect(reattached.datasets).toEqual(state.datasets);
  const service = "backtest-factor-after-series";
  const deployed = await rpc(page.request, "node.deploy", {
    id: "local",
    service,
    kind: "task",
    port: "0",
  });
  expect(deployed.dataset_series).toEqual(state.dataset_series);
  const switched = await rpc(page.request, "node.data_tasks.attach", { id: "local", service });
  checkSnapshot("new store after dominant series", switched);
  expect(switched.datasets).toEqual([]);
  expect(switched.dataset_series).toEqual([]);
  const restored = await rpc(page.request, "node.data_tasks.attach", {
    id: "local",
    service: originalService,
  });
  expect(restored.dataset_series).toEqual([]);
  expect((await rpc(page.request, "task.result", { id: task.id })).task_result.result).toEqual(
    result,
  );
});
