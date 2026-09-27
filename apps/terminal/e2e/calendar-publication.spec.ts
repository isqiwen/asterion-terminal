import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("calendar publication survives source deletion and stays outside research task list", async ({
  page,
}) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-calendar-ui-"));
  const csv = join(folder, "ticks.csv"),
    calendar = join(folder, "settlements.csv");
  await writeFile(
    csv,
    "timestamp_ns,price,quantity\n" +
      [100, 101, 102, 101, 100, 101, 103]
        .map((price, index) => `${1790298000000000000n + BigInt(index) * 1000000000n},${price},1`)
        .join("\n") +
      "\n",
  );
  await writeFile(
    calendar,
    "trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source\n2026-09-25,2026-09-25T09:00:00+08:00,2026-09-25T15:00:00+08:00,105,fixture schedule,fixture settlement\n",
  );
  try {
    await page.goto("/");
    await page.getByRole("button", { name: "数据", exact: true }).click();
    if ((await page.locator("details.data-import").getAttribute("open")) === null)
      await page.getByText("导入 CSV", { exact: true }).click();
    for (const [label, value] of [
      ["CSV 文件路径", csv],
      ["品种代码", "rb"],
      ["实际合约", "rb2610"],
      ["交割月份", "2026-10"],
      ["价格步长", "1"],
      ["每手乘数", "10"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "校验并预览", exact: true }).click();
    await expect(page.getByText("文件校验完成，可在市场工作区查看历史成交。")).toBeVisible();
    const section = page.getByRole("region", { name: "交易日与结算表", exact: true });
    await section.getByLabel("结算表 CSV 路径").fill(calendar);
    const ids = () => section.locator(".publication-row details code").allTextContents();
    const before = new Set(await ids());
    await section.getByRole("button", { name: "发布结算表", exact: true }).click();
    await expect.poll(async () => (await ids()).filter(id => !before.has(id)).length).toBe(1);
    const id = (await ids()).find(id => !before.has(id))!;
    await rm(calendar);
    await page.reload();
    await page.getByRole("button", { name: "数据", exact: true }).click();
    const row = section.locator(".publication-row").filter({ hasText: id });
    await expect(row.getByText("已发布", { exact: true })).toBeVisible({ timeout: 20000 });
    await row.getByRole("button", { name: "查看结算表", exact: true }).click();
    const result = section.getByRole("region", { name: "结算表内容", exact: true });
    await expect(result.getByRole("cell", { name: "105", exact: true })).toBeVisible();
    await result.getByText("来源说明", { exact: true }).click();
    await expect(result.getByText("fixture schedule", { exact: true })).toBeVisible();
    await page.setViewportSize({ width: 800, height: 900 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await section.screenshot({ path: join(__dirname, "../test-results/calendar-publication.png") });
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(page.getByRole("region", { name: "研究任务", exact: true })).not.toContainText(id);
    const research = page.getByRole("region", { name: "期货研究", exact: true });
    await research.getByLabel("日程来源", { exact: true }).selectOption(id);
    await expect(research.getByLabel("交易日", { exact: true })).toHaveCount(0);
    for (const [label, value] of [
      ["快均线", "1"],
      ["慢均线", "3"],
      ["目标手数", "1"],
      ["初始资金", "10000"],
      ["每手保证金", "100"],
      ["开仓手续费", "2"],
      ["平今手续费", "3"],
      ["平昨手续费", "4"],
      ["单笔数量上限", "100"],
      ["总持仓量上限", "100"],
      ["在途委托数上限", "100"],
    ])
      await research.getByLabel(label, { exact: true }).fill(value);
    const researchIds = () => research.locator("tbody tr code").allTextContents();
    const previous = new Set(await researchIds());
    await research.getByRole("button", { name: "开始回测", exact: true }).click();
    await expect
      .poll(async () => (await researchIds()).filter(value => !previous.has(value)).length)
      .toBe(1);
    const backtestId = (await researchIds()).find(value => !previous.has(value))!;
    await page.reload();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    const backtest = research.getByRole("row").filter({ hasText: backtestId });
    await expect(backtest.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
    await backtest.getByRole("button", { name: "查看结果", exact: true }).click();
    const backtestResult = research.getByRole("region", { name: "回测结果", exact: true });
    await backtestResult.getByText("实验参数", { exact: true }).click();
    await backtestResult.getByText("结算表版本", { exact: true }).click();
    await expect(backtestResult.getByText("settlements.csv", { exact: true })).toBeVisible();
    await expect(backtestResult.getByText("fixture settlement", { exact: true })).toBeVisible();
    await research.getByLabel("日程来源", { exact: true }).selectOption(id);
    await research.getByLabel("日程来源", { exact: true }).selectOption("");
    await expect(research.getByLabel("交易日", { exact: true })).toHaveValue("");
  } finally {
    await rm(folder, { recursive: true, force: true });
  }
});
