import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";

// Daily bars are the market-wide daily table's own: nothing is downloaded or
// chosen for them. This journey has a node of its own; the dominant series
// spec ends with a second task store deployed on its node.
test("a contract's days and a product's months backtest from the daily table", async ({ page }) => {
  test.setTimeout(60000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // The series reads every month the daily table has of the product. ag2610
  // holds the open interest until ag2701 appears with more of it, settling at
  // twice the price: the roll falls on 2026-09-25.
  await seedHistory(page.request, [100], "daily-near", {
    product: "ag",
    minuteDays: ["2026-09-25"],
    dailyDays: ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"],
  });
  await seedHistory(page.request, [100], "daily-far", {
    product: "ag",
    keep: true,
    month: "2027-01",
    minuteDays: ["2026-09-25"],
    dailyDays: ["2026-09-24", "2026-09-25", "2026-09-28"],
    openInterest: 200,
    settlement: 220,
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "新建回测", exact: true }).click();
  const selected = workspace.getByRole("list", { name: "已选合约" });
  // One contract: the days it traded are its bars, and nothing is downloaded
  // or chosen for it.
  const contract = workspace.getByRole("form", { name: "日线合约", exact: true });
  await contract.getByLabel("合约", { exact: true }).fill("SHFE/ag/2026-10");
  await contract.getByLabel("开始交易日", { exact: true }).fill("2026-09-21");
  await contract.getByLabel("结束交易日", { exact: true }).fill("2026-09-25");
  await contract.getByLabel("最小变动价位", { exact: true }).fill("1");
  await contract.getByLabel("合约乘数", { exact: true }).fill("10");
  await contract.getByRole("button", { name: "使用这个合约的日线", exact: true }).click();
  await expect(selected).toContainText("SHFE · ag2610 · 日线 · 5 根 · 5 个交易日");
  await expect(selected).toContainText("日线和结算价取自全市场日线表");
  const one = (await rpc(page.request, "runtime.snapshot")).datasets;
  expect(one).toHaveLength(1);
  expect(one[0].source_dataset_ids).toEqual([]);
  expect(one[0].interval_minutes).toBe(0);
  await selected.getByRole("button", { name: "移除 SHFE · ag2610", exact: true }).click();
  await expect(selected.locator("li")).toHaveCount(0);
  // The product: the data service finds the months that traded in the range.
  await workspace.getByRole("button", { name: "日线主力连续", exact: true }).click();
  const series = workspace.getByRole("form", { name: "日线主力连续", exact: true });
  await series.getByLabel("品种", { exact: true }).fill("SHFE/ag");
  await series.getByLabel("开始交易日", { exact: true }).fill("2026-09-21");
  await series.getByLabel("结束交易日", { exact: true }).fill("2026-09-28");
  await series.getByLabel("最小变动价位", { exact: true }).fill("1");
  await series.getByLabel("合约乘数", { exact: true }).fill("10");
  await series.getByRole("button", { name: "使用这个品种的日线主力连续", exact: true }).click();
  await expect(selected).toContainText("SHFE · ag 主力连续");
  // ag2610 from the second day to the roll day, ag2701 from the day before it
  // takes over.
  await expect(selected).toContainText("日线 · 7 根 · 2 个月份");
  await selected.getByText("换月日程（1 次换月）", { exact: true }).click();
  const rolls = selected.getByRole("table", { name: "SHFE · ag 主力连续 换月日程" });
  await expect(rolls.locator("tbody tr")).toHaveText([
    /2026-09-22\s*ag2610\s*2/,
    /2026-09-25\s*ag2701\s*1/,
  ]);
  const state = await rpc(page.request, "runtime.snapshot");
  checkSnapshot("daily dominant series selection", state);
  expect(state.dataset_series).toHaveLength(1);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(
    workspace.getByRole("region", { name: "SHFE · ag 主力连续 保证金与手续费" }),
  ).toBeVisible();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["滑点（跳）", "0"],
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
  // The task froze the months the table named on the day it was submitted:
  // both are read from the daily table, and every day of the series settles.
  const task = (await rpc(page.request, "runtime.snapshot")).task_service.tasks.at(-1);
  const evidence = (await rpc(page.request, "task.result", { id: task.id })).task_result;
  expect(
    evidence.experiment.paper.contracts.map(
      (item: { dataset: { contract: { symbol: string }; source_dataset_ids: string[] } }) => [
        item.dataset.contract.symbol,
        item.dataset.source_dataset_ids,
      ],
    ),
  ).toEqual([
    ["ag2610", []],
    ["ag2701", []],
  ]);
  expect(evidence.result.settlements).toHaveLength(5);
  // The details say where bars without an archive version came from.
  await row.first().getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "回测结果", exact: true });
  await result.getByText("实验参数", { exact: true }).click();
  await expect(result.getByRole("region", { name: "SHFE · ag2610", exact: true })).toContainText(
    "取自全市场日线表",
  );
});
