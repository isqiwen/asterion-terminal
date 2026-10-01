import { seedDataset } from "./dataset-fixture";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { join } from "node:path";

test("Agent runs a bar backtest and restores its evidence", async ({ page }) => {
  await page.goto("/");
  await seedDataset(page.request, [100, 101, 102, 101, 104, 103, 102, 103], "research");
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  const research = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(research.getByText("研究服务已连接", { exact: true })).toBeVisible();
  const selected = research.getByRole("list", { name: "已选合约" });
  await expect(selected).toContainText("SHFE · rb2610");
  await expect(selected).toContainText("8 根 · 1 个交易日");
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await research.getByLabel(label, { exact: true }).fill(value);
  // Drafts survive switching workspaces.
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await expect(research.getByLabel("快均线", { exact: true })).toHaveValue("1");
  await expect(research.getByLabel("初始资金", { exact: true })).toHaveValue("10000");
  const taskIds = () =>
    research
      .getByRole("region", { name: "研究任务", exact: true })
      .locator("tbody tr code")
      .allTextContents();
  const before = new Set(await taskIds());
  await research.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(research.getByText("任务已提交，可关闭窗口。", { exact: true })).toBeVisible();
  await expect.poll(async () => (await taskIds()).filter(id => !before.has(id)).length).toBe(1);
  const taskId = (await taskIds()).find(id => !before.has(id))!;
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  const row = research
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: taskId });
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  // A full reload starts a fresh draft scope; persisted task results remain available.
  await expect(research.getByLabel("初始资金", { exact: true })).toHaveValue("");
  await research.getByLabel("快均线", { exact: true }).fill("9");
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = research.getByRole("region", { name: "回测结果", exact: true });
  await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
  await result.getByText("实验参数", { exact: true }).click();
  const parameter = (label: string) =>
    result
      .locator(".experiment-fields > div")
      .filter({ has: page.getByText(label, { exact: true }) })
      .locator("dd");
  await expect(parameter("快均线")).toHaveText("1");
  await expect(parameter("初始资金")).toHaveText("10000");
  await expect(parameter("平今手续费")).toHaveText("3");
  await expect(parameter("输入 K 线")).toHaveText("8");
  await expect(parameter("K 线下载任务")).toHaveText("research-bars");
  await expect(parameter("结算价下载任务")).toHaveText("research-settlement");
  await expect(parameter("交易日范围")).toHaveText("2026-09-25 – 2026-09-25");
  await result.getByText("逐日结算", { exact: true }).click();
  const settlements = result.locator(".research-settlements tbody tr");
  await expect(settlements).toHaveCount(1);
  await expect(settlements.first()).toContainText("110");
  await expect(research.getByLabel("快均线", { exact: true })).toHaveValue("9");
  await result
    .locator(".research-experiment")
    .screenshot({ path: join(__dirname, "../test-results/research-evidence.png") });
  await page.setViewportSize({ width: 800, height: 900 });
  await expect(
    research.getByRole("button", { name: "查看结果", exact: true }).last(),
  ).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(
    true,
  );
  page = await openSettingsWindow(page);
  await page.getByLabel("语言", { exact: true }).selectOption("en-US");
  page = await closeSettingsWindow(page);
  await page.getByRole("button", { name: "Research", exact: true }).click();
  const english = page.getByRole("region", { name: "Backtest Result", exact: true });
  // The parameters <details> opened in Chinese stays open across the
  // language switch; clicking it again would collapse it.
  if ((await english.locator("details.research-experiment").getAttribute("open")) === null)
    await english.getByText("Experiment Parameters", { exact: true }).click();
  await expect(english).not.toContainText(/\p{Script=Han}/u);
  await expect(english.getByText("Input bars", { exact: true })).toBeVisible();
});

test("a portfolio backtest settles every contract on one account", async ({ page }) => {
  await page.goto("/");
  const prices = [100, 101, 102, 101, 104, 103, 102, 103];
  await seedDataset(page.request, prices, "portfolio-rb");
  await seedDataset(page.request, prices, "portfolio-hc", { product: "hc", keep: true });
  await page.reload();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  const research = page.getByRole("region", { name: "期货研究", exact: true });
  await research.getByRole("button", { name: "均线回测", exact: true }).click();
  const selected = research.getByRole("list", { name: "已选合约" });
  await expect(selected.getByRole("listitem")).toHaveCount(2);
  await expect(selected).toContainText("SHFE · hc2610");
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await research.getByLabel(label, { exact: true }).fill(value);
  for (const symbol of ["rb2610", "hc2610"]) {
    const costs = research.getByRole("region", { name: `SHFE · ${symbol} 保证金与手续费` });
    for (const [label, value] of [
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
    ])
      await costs.getByLabel(label, { exact: true }).fill(value);
  }
  await research.getByRole("button", { name: "开始回测", exact: true }).click();
  await expect(research.getByText("任务已提交，可关闭窗口。", { exact: true })).toBeVisible();
  const row = research
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb2610 + SHFE/hc2610" });
  await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await row.getByRole("button", { name: "查看结果", exact: true }).first().click();
  const result = research.getByRole("region", { name: "回测结果", exact: true });
  await result.getByText("逐日结算", { exact: true }).click();
  const settlements = result.locator(".research-settlements tbody tr");
  await expect(settlements).toHaveCount(2);
  await expect(settlements.nth(0)).toContainText("rb2610");
  await expect(settlements.nth(1)).toContainText("hc2610");
  await result.getByText("实验参数", { exact: true }).click();
  await expect(result.getByRole("region", { name: "SHFE · hc2610" })).toContainText("100");
  // Factor analysis studies one contract and says so instead of submitting.
  await research.getByRole("button", { name: "因子分析", exact: true }).click();
  await expect(
    research.getByText("因子分析只研究一个合约，请只保留一个数据集。", { exact: true }),
  ).toBeVisible();
  await research.getByRole("button", { name: "移除 SHFE · hc2610", exact: true }).click();
  await expect(selected.getByRole("listitem")).toHaveCount(1);
});
