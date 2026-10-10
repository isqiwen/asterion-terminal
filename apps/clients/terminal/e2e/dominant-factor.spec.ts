import { rpc, seedHistory } from "./dataset-fixture";
import { test, expect } from "./test";
import { checkSnapshot } from "./snapshot-schema";
import { join } from "node:path";

// In a file of its own: the dominant series backtest ends with a second
// service pair that fills the node, and this needs a worker.
test("a factor reads a product's months as one dominant series", async ({ page }) => {
  test.setTimeout(60000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // Eight bars a day. rb2610 holds the open interest until rb2701 appears
  // with more of it at twice the price: the roll falls on 2026-09-25.
  await seedHistory(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "factor-near",
    { minuteDays: ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"] },
  );
  await seedHistory(
    page.request,
    Array.from({ length: 24 }, (_, i) => 250 + 2 * i - (i % 4)),
    "factor-far",
    {
      keep: true,
      month: "2027-01",
      minuteDays: ["2026-09-24", "2026-09-25", "2026-09-28"],
      openInterest: 200,
      settlement: 220,
    },
  );
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("任务服务已连接", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await workspace.getByRole("button", { name: "主力连续", exact: true }).click();
  await workspace
    .getByLabel("品种", { exact: true })
    .selectOption({ label: "SHFE/rb · 1 分钟 · tushare.ft_mins · 2 个月份" });
  const series = workspace.getByRole("region", { name: "主力连续", exact: true });
  await series.getByLabel("最小变动价位").fill("1");
  await series.getByLabel("合约乘数").fill("10");
  await series.getByRole("button", { name: "使用主力连续（2 个月份）", exact: true }).click();
  await expect(workspace.getByRole("list", { name: "已选合约" })).toContainText(
    "SHFE · rb 主力连续",
  );
  // Two selected months are one series, not two contracts to compare.
  await expect(
    workspace.getByText(
      "主力连续 · 每个交易日取当时主力月份的 K 线，收盘价按换月比例调整到最新月份的水平",
      { exact: true },
    ),
  ).toBeVisible();
  // rb2701 has no bars before it becomes dominant and nothing follows it: no
  // day of this series can compare two months, and the page says so.
  await workspace.getByLabel("因子", { exact: true }).selectOption("term_structure");
  await expect(workspace.getByText(/所选主力连续里有的没有期限结构数据/)).toBeVisible();
  await expect(workspace.getByRole("button", { name: "开始分析", exact: true })).toBeDisabled();
  await workspace.getByLabel("因子", { exact: true }).selectOption("momentum");
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("1");
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb (rb2610-rb2701)" });
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  // Three days of rb2610 from 2026-09-22 and two of rb2701: 40 bars, less
  // the window of 2 and the horizon of 1.
  const whole = result.getByRole("region", { name: "全样本", exact: true });
  await expect(whole.getByText("37", { exact: true })).toBeVisible();
  await result.getByText("实验参数", { exact: true }).click();
  const details = result.getByRole("region", { name: "SHFE · rb · 主力连续", exact: true });
  const parameter = (label: string) =>
    details
      .locator(".experiment-fields > div")
      .filter({ has: page.getByText(label, { exact: true }) })
      .locator("dd");
  await expect(parameter("月份合约")).toHaveText("rb2610 · rb2701");
  await expect(parameter("换月")).toHaveText("2026-09-22 rb2610 ×2 · 2026-09-25 rb2701 ×1");
  await expect(parameter("输入 K 线")).toHaveText("40");
  const state = await rpc(page.request, "runtime.snapshot");
  const evidence = state.task_result;
  checkSnapshot("dominant series factor", state);
  expect(evidence.experiment.series[0].kind).toBe("dominant");
  expect(evidence.result.samples).toHaveLength(37);
  // The first sample is rb2610's third bar of 2026-09-22. Its closes that
  // day begin 110, 109, 111, 113 and are read doubled.
  expect(evidence.result.samples[0].value).toBeCloseTo(222 / 220 - 1, 12);
  expect(evidence.result.samples[0].forward_return).toBeCloseTo(226 / 222 - 1, 12);
  await page.screenshot({ path: join(__dirname, "../test-results/factor-dominant-series.png") });
});

test("the term structure of a dominant series is evaluated as a factor", async ({ page }) => {
  test.setTimeout(60000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  // hc2610 keeps the open interest on all five days and settles at 110;
  // hc2701 trades beside it at 100. Each day from the second begins knowing
  // both settlements of the day before.
  const days = ["2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"];
  await seedHistory(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "term-near",
    { product: "hc", minuteDays: days },
  );
  await seedHistory(
    page.request,
    Array.from({ length: 40 }, (_, i) => 90 + i),
    "term-far",
    {
      product: "hc",
      keep: true,
      month: "2027-01",
      minuteDays: days,
      openInterest: 50,
      settlement: 100,
    },
  );
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("任务服务已连接", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await workspace.getByRole("button", { name: "主力连续", exact: true }).click();
  await workspace
    .getByLabel("品种", { exact: true })
    .selectOption({ label: "SHFE/hc · 1 分钟 · tushare.ft_mins · 2 个月份" });
  const series = workspace.getByRole("region", { name: "主力连续", exact: true });
  await series.getByLabel("最小变动价位").fill("1");
  await series.getByLabel("合约乘数").fill("10");
  await series.getByRole("button", { name: "使用主力连续（2 个月份）", exact: true }).click();
  await expect(workspace.getByRole("list", { name: "已选合约" })).toContainText(
    "SHFE · hc 主力连续",
  );
  await workspace.getByLabel("因子", { exact: true }).selectOption("term_structure");
  await expect(
    workspace.getByText(/^期限结构 · 每个交易日开始时已知的年化近远月价差/),
  ).toBeVisible();
  await expect(workspace.getByRole("alert")).toHaveCount(0);
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("1");
  await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("1");
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/hc (hc2610-hc2610)" });
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await expect(result).toContainText("按 K 线计算 · 期限结构 · 全样本评价");
  // Four days have a term point: 32 bars, less the window of 1 and the horizon of 1.
  const whole = result.getByRole("region", { name: "全样本", exact: true });
  await expect(whole.getByText("30", { exact: true })).toBeVisible();
  await result.getByText("实验参数", { exact: true }).click();
  const details = result.getByRole("region", { name: "SHFE · hc · 主力连续", exact: true });
  const parameter = (label: string) =>
    details
      .locator(".experiment-fields > div")
      .filter({ has: page.getByText(label, { exact: true }) })
      .locator("dd");
  await expect(parameter("有期限结构的交易日")).toHaveText("4");
  await expect(parameter("输入 K 线")).toHaveText("32");
  const state = await rpc(page.request, "runtime.snapshot");
  const evidence = state.task_result;
  checkSnapshot("term structure factor", state);
  expect(evidence.experiment.factor).toBe("term_structure");
  expect(evidence.result.samples).toHaveLength(30);
  // 110 over 100 less one, three delivery months apart: 0.1 x 12 / 3.
  expect(evidence.result.samples[0].value).toBeCloseTo(0.4, 12);
  await page.screenshot({ path: join(__dirname, "../test-results/factor-term-structure.png") });
});
